/*
 * MatonOS: replaces <hal/ffi.h> from packages/modules/Bluetooth/offload/hal.
 * Same C++ interface the stock HAL implements, but served by a plain NDK AIDL
 * adapter (aidl_service.cpp) instead of the Rust offload proxy, whose Soong
 * visibility excludes device trees and which we do not need (no HCI offload).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <aidl/android/hardware/bluetooth/IBluetoothHciCallbacks.h>

namespace aidl::android::hardware::bluetooth::hal {

// Same values as the offload ffi enum; aliasing avoids a second Status type.
using Status = ::aidl::android::hardware::bluetooth::Status;

class IBluetoothHciCallbacks {
  public:
    explicit IBluetoothHciCallbacks(
            std::shared_ptr<::aidl::android::hardware::bluetooth::IBluetoothHciCallbacks> cb)
        : cb_(std::move(cb)) {}
    void initializationComplete(Status status);
    void hciEventReceived(std::vector<uint8_t> data);
    void aclDataReceived(std::vector<uint8_t> data);
    void scoDataReceived(std::vector<uint8_t> data);
    void isoDataReceived(std::vector<uint8_t> data);

  private:
    std::shared_ptr<::aidl::android::hardware::bluetooth::IBluetoothHciCallbacks> cb_;
};

class IBluetoothHci {
  public:
    virtual ~IBluetoothHci() = default;
    virtual void initialize(const std::shared_ptr<IBluetoothHciCallbacks>& callbacks) = 0;
    virtual void close() = 0;
    virtual void sendHciCommand(const std::vector<uint8_t>& data) = 0;
    virtual void sendAclData(const std::vector<uint8_t>& data) = 0;
    virtual void sendScoData(const std::vector<uint8_t>& data) = 0;
    virtual void sendIsoData(const std::vector<uint8_t>& data) = 0;
    virtual void clientDied() {}
    virtual void dump(int fd) { (void)fd; }
};

// Registers IBluetoothHci/default. Returns false if service registration failed.
bool IBluetoothHci_addService(IBluetoothHci* hci);

}  // namespace aidl::android::hardware::bluetooth::hal
