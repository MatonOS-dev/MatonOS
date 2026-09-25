#include "core-impl/SoundDose.h"

namespace aidl::android::hardware::audio::core::sounddose {
ndk::ScopedAStatus SoundDose::setOutputRs2UpperBound(float value) {
    if (value < 80.0f || value > 100.0f) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mRs2Value = value;
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus SoundDose::getOutputRs2UpperBound(float* result) {
    *result = mRs2Value;
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus SoundDose::registerSoundDoseCallback(
        const std::shared_ptr<ISoundDose::IHalSoundDoseCallback>& callback) {
    return callback ? ndk::ScopedAStatus::ok()
                    : ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}
}  // namespace aidl::android::hardware::audio::core::sounddose
