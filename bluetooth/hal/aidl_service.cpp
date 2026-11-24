/*
 * MatonOS: NDK AIDL front end for the forked HCI HAL (see hci_interface.h).
 * Forwards every call; BluetoothHci owns all state, including replacing a
 * stale session. A dead client (stack crash) is reported via clientDied().
 */
#define LOG_TAG "aidl.android.hardware.bluetooth.service.matonos"

#include "hci_interface.h"

#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <android/binder_manager.h>
#include <log/log.h>

#include <mutex>

namespace aidl::android::hardware::bluetooth::hal {
namespace {

using AidlCallbacks = ::aidl::android::hardware::bluetooth::IBluetoothHciCallbacks;
// Inside AidlHci the unqualified names resolve to the inherited AIDL types.
using HalHci = ::aidl::android::hardware::bluetooth::hal::IBluetoothHci;
using HalCallbacks = ::aidl::android::hardware::bluetooth::hal::IBluetoothHciCallbacks;

void logIfFailed(const ndk::ScopedAStatus& status, const char* what) {
    if (!status.isOk()) ALOGW("callback %s failed: %s", what, status.getDescription().c_str());
}

class AidlHci : public ::aidl::android::hardware::bluetooth::BnBluetoothHci {
  public:
    explicit AidlHci(HalHci* hci)
        : hci_(hci), death_(AIBinder_DeathRecipient_new(&AidlHci::onDeath)) {}

    ndk::ScopedAStatus initialize(const std::shared_ptr<AidlCallbacks>& cb) override {
        if (!cb) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (linked_.get() != nullptr) AIBinder_unlinkToDeath(linked_.get(), death_.get(), this);
            linked_ = cb->asBinder();
            const binder_status_t s = AIBinder_linkToDeath(linked_.get(), death_.get(), this);
            if (s != STATUS_OK) ALOGW("linkToDeath failed: %d", s);
        }
        hci_->initialize(std::make_shared<HalCallbacks>(cb));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus close() override {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (linked_.get() != nullptr) AIBinder_unlinkToDeath(linked_.get(), death_.get(), this);
            linked_ = nullptr;
        }
        hci_->close();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus sendHciCommand(const std::vector<uint8_t>& d) override {
        hci_->sendHciCommand(d);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus sendAclData(const std::vector<uint8_t>& d) override {
        hci_->sendAclData(d);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus sendScoData(const std::vector<uint8_t>& d) override {
        hci_->sendScoData(d);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus sendIsoData(const std::vector<uint8_t>& d) override {
        hci_->sendIsoData(d);
        return ndk::ScopedAStatus::ok();
    }
    binder_status_t dump(int fd, const char**, uint32_t) override {
        hci_->dump(fd);
        return STATUS_OK;
    }

  private:
    static void onDeath(void* cookie) {
        auto* self = static_cast<AidlHci*>(cookie);
        ALOGI("Bluetooth stack has died");
        {
            std::lock_guard<std::mutex> guard(self->mutex_);
            self->linked_ = nullptr;
        }
        self->hci_->clientDied();
    }

    HalHci* hci_;
    ndk::ScopedAIBinder_DeathRecipient death_;
    std::mutex mutex_;
    ndk::SpAIBinder linked_;
};

}  // namespace

void IBluetoothHciCallbacks::initializationComplete(Status status) {
    logIfFailed(cb_->initializationComplete(status),
                "initializationComplete");
}
void IBluetoothHciCallbacks::hciEventReceived(std::vector<uint8_t> data) {
    logIfFailed(cb_->hciEventReceived(data), "hciEventReceived");
}
void IBluetoothHciCallbacks::aclDataReceived(std::vector<uint8_t> data) {
    logIfFailed(cb_->aclDataReceived(data), "aclDataReceived");
}
void IBluetoothHciCallbacks::scoDataReceived(std::vector<uint8_t> data) {
    logIfFailed(cb_->scoDataReceived(data), "scoDataReceived");
}
void IBluetoothHciCallbacks::isoDataReceived(std::vector<uint8_t> data) {
    logIfFailed(cb_->isoDataReceived(data), "isoDataReceived");
}

bool IBluetoothHci_addService(IBluetoothHci* hci) {
    // Process-lifetime singleton, like the stock HAL's service object.
    static std::shared_ptr<AidlHci> service = ndk::SharedRefBase::make<AidlHci>(hci);
    const std::string instance = std::string(AidlHci::descriptor) + "/default";
    const binder_status_t s = AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (s != STATUS_OK) ALOGE("addService %s failed: %d", instance.c_str(), s);
    return s == STATUS_OK;
}

}  // namespace aidl::android::hardware::bluetooth::hal
