/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "MatonBluetoothHci"
#include "BluetoothHci.h"

#include <aidl/android/hardware/bluetooth/Status.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/socket.h>
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

struct sockaddr_hci { sa_family_t hci_family; uint16_t hci_dev; uint16_t hci_channel; };
struct RfkillEvent { uint32_t index; uint8_t type, op, soft, hard; } __attribute__((packed));
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
  RfkillEvent event{static_cast<uint32_t>(index), 2, 2,
                    static_cast<uint8_t>(block ? 1 : 0), 0};
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ >= 0) { cb->initializationComplete(Status::ALREADY_INITIALIZED); return ndk::ScopedAStatus::ok(); }
  char value[92] = {};
  if (__system_property_get("vendor.maton.bluetooth.hci_index", value) <= 0) {
    cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  const int index = atoi(value);
  char rfkill_value[92] = {};
  __system_property_get("vendor.maton.bluetooth.rfkill_index", rfkill_value);
  char virtual_value[92] = {};
  __system_property_get("vendor.maton.bluetooth.virtual", virtual_value);
  hci_index_ = index;
  virtual_controller_ = strcmp(virtual_value, "1") == 0;
  rfkill_index_ = virtual_controller_ ? -1 : atoi(rfkill_value);
  if (!virtual_controller_) {
    SetRfkill(rfkill_index_, false);
    usleep(100000);
  }
  const int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
  if (fd < 0) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "HCI socket: %s", strerror(errno));
    if (!virtual_controller_) SetRfkill(rfkill_index_, true);
    cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  sockaddr_hci addr{};
  addr.hci_family = AF_BLUETOOTH; addr.hci_dev = static_cast<uint16_t>(index); addr.hci_channel = HCI_CHANNEL_USER;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "bind hci%d user channel: %s", index, strerror(errno));
    ::close(fd);
    if (!virtual_controller_) SetRfkill(rfkill_index_, true);
    cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE); return ndk::ScopedAStatus::ok();
  }
  fd_ = fd; callbacks_ = cb; running_ = true;
  reader_ = std::thread(&BluetoothHci::receiveLoop, this);
  cb->initializationComplete(Status::SUCCESS);
  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "bound HCI user channel to hci%d", index);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::close() {
  running_ = false;
  int fd;
  { std::lock_guard<std::mutex> lock(mutex_); fd = fd_; fd_ = -1; callbacks_.reset(); }
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
  ssize_t sent; do { sent = write(fd_, frame.data(), frame.size()); } while (sent < 0 && errno == EINTR);
  if (sent != static_cast<ssize_t>(frame.size()))
    return ndk::ScopedAStatus::fromServiceSpecificError(errno ? errno : EIO);
  return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& p) { return sendPacket(HCI_COMMAND_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& p) { return sendPacket(HCI_ACLDATA_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& p) { return sendPacket(HCI_SCODATA_PKT, p); }
ndk::ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>& p) { return sendPacket(HCI_ISODATA_PKT, p); }

void BluetoothHci::receiveLoop() {
  uint8_t buf[65536];
  while (running_) {
    int fd; { std::lock_guard<std::mutex> lock(mutex_); fd = fd_; }
    if (fd < 0) break;
    pollfd pfd{fd, POLLIN, 0}; int result;
    do { result = poll(&pfd, 1, 1000); } while (result < 0 && errno == EINTR);
    if (result == 0) continue;
    if (result < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    ssize_t size = read(fd, buf, sizeof(buf)); if (size <= 1) continue;
    std::shared_ptr<IBluetoothHciCallbacks> cb;
    { std::lock_guard<std::mutex> lock(mutex_); cb = callbacks_; }
    if (!cb) continue;
    std::vector<uint8_t> data(buf + 1, buf + size);
    switch (buf[0]) {
      case HCI_EVENT_PKT: cb->hciEventReceived(data); break;
      case HCI_ACLDATA_PKT: cb->aclDataReceived(data); break;
      case HCI_SCODATA_PKT: cb->scoDataReceived(data); break;
      case HCI_ISODATA_PKT: cb->isoDataReceived(data); break;
      default: __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "unknown HCI packet type %u", buf[0]);
    }
  }
}
} // namespace maton
