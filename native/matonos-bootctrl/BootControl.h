#pragma once

#include <aidl/android/hardware/boot/BnBootControl.h>

namespace matonos::bootctrl {

class BootControl final : public aidl::android::hardware::boot::BnBootControl {
  public:
    ndk::ScopedAStatus getActiveBootSlot(int32_t* result) override;
    ndk::ScopedAStatus getCurrentSlot(int32_t* result) override;
    ndk::ScopedAStatus getNumberSlots(int32_t* result) override;
    ndk::ScopedAStatus getSnapshotMergeStatus(
        aidl::android::hardware::boot::MergeStatus* result) override;
    ndk::ScopedAStatus getSuffix(int32_t slot, std::string* result) override;
    ndk::ScopedAStatus isSlotBootable(int32_t slot, bool* result) override;
    ndk::ScopedAStatus isSlotMarkedSuccessful(int32_t slot, bool* result) override;
    ndk::ScopedAStatus markBootSuccessful() override;
    ndk::ScopedAStatus setActiveBootSlot(int32_t slot) override;
    ndk::ScopedAStatus setSlotAsUnbootable(int32_t slot) override;
    ndk::ScopedAStatus setSnapshotMergeStatus(
        aidl::android::hardware::boot::MergeStatus status) override;
};

}  // namespace matonos::bootctrl
