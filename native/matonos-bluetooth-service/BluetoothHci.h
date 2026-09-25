/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace maton {
class BluetoothHci final : public aidl::android::hardware::bluetooth::BnBluetoothHci {
 public:
  ~BluetoothHci() override;
  ndk::ScopedAStatus initialize(const std::shared_ptr<aidl::android::hardware::bluetooth::IBluetoothHciCallbacks>& cb) override;
  ndk::ScopedAStatus close() override;
  ndk::ScopedAStatus sendHciCommand(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendAclData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendScoData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendIsoData(const std::vector<uint8_t>& packet) override;
 private:
  ndk::ScopedAStatus sendPacket(uint8_t type, const std::vector<uint8_t>& packet);
  void receiveLoop();
  std::mutex mutex_;
  int fd_ = -1;
  int hci_index_ = -1;
  int rfkill_index_ = -1;
  bool virtual_controller_ = false;
  std::atomic<bool> running_{false};
  std::thread reader_;
  std::shared_ptr<aidl::android::hardware::bluetooth::IBluetoothHciCallbacks> callbacks_;
};
} // namespace maton
