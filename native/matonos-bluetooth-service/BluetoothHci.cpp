/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "MatonBluetoothHci"
#include "BluetoothHci.h"

#include <aidl/android/hardware/bluetooth/Status.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/rfkill.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define HCI_CHANNEL_USER 1
#define HCI_COMMAND_PKT 0x01
#define HCI_ACLDATA_PKT 0x02
#define HCI_SCODATA_PKT 0x03
#define HCI_EVENT_PKT 0x04
#define HCI_ISODATA_PKT 0x05

// The NDK sysroot does not expose linux/bluetooth.h; this is the stable
// sockaddr_hci UAPI layout used by the Bluetooth user channel.
struct sockaddr_hci { sa_family_t hci_family; uint16_t hci_dev; uint16_t hci_channel; };

using aidl::android::hardware::bluetooth::IBluetoothHciCallbacks;
using aidl::android::hardware::bluetooth::Status;

namespace {
bool SetRfkill(int index, bool block) {
  if (index < 0) return true;
  const int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "open rfkill: %s", strerror(errno));
    return false;
  }
  rfkill_event event{};
  event.idx = static_cast<uint32_t>(index); event.type = RFKILL_TYPE_BLUETOOTH;
  event.op = RFKILL_OP_CHANGE; event.soft = block ? 1 : 0;
  const bool okay = write(fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event));
  if (!okay) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "rfkill update: %s", strerror(errno));
  close(fd);
  return okay;
}
} // namespace

namespace maton {
BluetoothHci::~BluetoothHci() { close(); }

ndk::ScopedAStatus BluetoothHci::initialize(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
  if (!cb) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  std::unique_lock<std::mutex> lock(mutex_);
  if (fd_ >= 0) {
    // Bluetooth can restart after a client crash without restarting this HAL.
    // Keep the existing exclusive channel and attach the new callback client.
    callbacks_ = cb;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "reusing active HCI user channel for hci%d", hci_index_);
    lock.unlock();
    auto status = cb->initializationComplete(Status::SUCCESS);
    if (!status.isOk()) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "callback init failed");
    return ndk::ScopedAStatus::ok();
  }
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("vendor.maton.bluetooth.hci_index", value) <= 0) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "Bluetooth manager has not published an HCI index");
    lock.unlock(); cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  char* end = nullptr;
  const long parsed_index = strtol(value, &end, 10);
  if (!end || *end || parsed_index < 0 || parsed_index > UINT16_MAX) {
    lock.unlock(); cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  const int index = static_cast<int>(parsed_index);
  char rfkill_value[PROP_VALUE_MAX] = {};
  const bool has_rfkill = __system_property_get("vendor.maton.bluetooth.rfkill_index", rfkill_value) > 0;
  char virtual_value[PROP_VALUE_MAX] = {};
  __system_property_get("vendor.maton.bluetooth.virtual", virtual_value);
  __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                      "initializing hci%d (virtual=%s)", index, virtual_value);
  hci_index_ = index;
  virtual_controller_ = strcmp(virtual_value, "1") == 0;
  char* rf_end = nullptr;
  long rf_index = has_rfkill ? strtol(rfkill_value, &rf_end, 10) : -1;
  rfkill_index_ = (!virtual_controller_ && has_rfkill && rf_end && !*rf_end && rf_index >= 0 && rf_index <= INT_MAX) ? static_cast<int>(rf_index) : -1;
  if (!virtual_controller_) {
    if (rfkill_index_ < 0) {
      __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "rfkill index missing; continuing without rfkill control");
    }
    if (!SetRfkill(rfkill_index_, false)) {
      lock.unlock(); cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
    }
    usleep(100000);
  }
  const int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
  if (fd < 0) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "HCI socket: %s", strerror(errno));
    if (!virtual_controller_) SetRfkill(rfkill_index_, true);
    lock.unlock(); cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  sockaddr_hci addr{};
  addr.hci_family = AF_BLUETOOTH; addr.hci_dev = static_cast<uint16_t>(index); addr.hci_channel = HCI_CHANNEL_USER;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "bind hci%d user channel: %s", index, strerror(errno));
    ::close(fd);
    if (!virtual_controller_) SetRfkill(rfkill_index_, true);
    lock.unlock(); cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  fd_ = fd; callbacks_ = cb; running_ = true;
  reader_ = std::thread(&BluetoothHci::receiveLoop, this);
  lock.unlock();
  auto status = cb->initializationComplete(Status::SUCCESS);
  if (!status.isOk()) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "callback init failed");
  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "bound HCI user channel to hci%d", index);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::close() {
  int fd;
  { std::lock_guard<std::mutex> lock(mutex_); running_ = false; fd = fd_; fd_ = -1; callbacks_.reset(); }
  if (fd >= 0) { shutdown(fd, SHUT_RDWR); ::close(fd); }
  if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
  if (!virtual_controller_) SetRfkill(rfkill_index_, true);
  hci_index_ = -1;
  rfkill_index_ = -1;
  virtual_controller_ = false;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendPacket(uint8_t type, const std::vector<uint8_t>& packet) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
  std::vector<uint8_t> frame; frame.reserve(packet.size() + 1); frame.push_back(type);
  frame.insert(frame.end(), packet.begin(), packet.end());
  size_t offset = 0;
  while (offset < frame.size()) {
    ssize_t sent = write(fd_, frame.data() + offset, frame.size() - offset);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return ndk::ScopedAStatus::fromServiceSpecificError(sent < 0 ? errno : EIO);
    offset += static_cast<size_t>(sent);
  }
  return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& p) { return sendPacket(HCI_COMMAND_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& p) { return sendPacket(HCI_ACLDATA_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& p) { return sendPacket(HCI_SCODATA_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>& p) { return sendPacket(HCI_ISODATA_PKT, p); }

void BluetoothHci::receiveLoop() {
  uint8_t buf[65536];
  while (running_) {
    int fd; { std::lock_guard<std::mutex> lock(mutex_); fd = fd_ >= 0 ? dup(fd_) : -1; }
    if (fd < 0) break;
    pollfd pfd{fd, POLLIN, 0}; int result;
    do { result = poll(&pfd, 1, 1000); } while (result < 0 && errno == EINTR);
    if (result == 0) { ::close(fd); continue; }
    if (result < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { ::close(fd); break; }
    ssize_t size = read(fd, buf, sizeof(buf)); ::close(fd); if (size <= 1) continue;
    std::shared_ptr<IBluetoothHciCallbacks> cb;
    { std::lock_guard<std::mutex> lock(mutex_); cb = callbacks_; }
    if (!cb) continue;
    std::vector<uint8_t> data(buf + 1, buf + size);
    ndk::ScopedAStatus status;
    switch (buf[0]) {
      case HCI_EVENT_PKT: status = cb->hciEventReceived(data); break;
      case HCI_ACLDATA_PKT: status = cb->aclDataReceived(data); break;
      case HCI_SCODATA_PKT: status = cb->scoDataReceived(data); break;
      case HCI_ISODATA_PKT: status = cb->isoDataReceived(data); break;
      default: __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "unknown HCI packet type %u", buf[0]);
    }
    if (!status.isOk() && status.getStatus() == STATUS_DEAD_OBJECT) {
      __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Bluetooth callback died; stopping HCI reader");
      running_ = false;
      break;
    }
  }
}
} // namespace maton
