/* MatonOS no-op sound-dose processor; copied AOSP DSP is intentionally omitted. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <aidl/android/hardware/audio/core/sounddose/BnSoundDose.h>
#include <aidl/android/media/audio/common/AudioDevice.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>

namespace aidl::android::hardware::audio::core::sounddose {
class StreamDataProcessorInterface {
  public:
    virtual ~StreamDataProcessorInterface() = default;
    virtual void startDataProcessor(uint32_t, uint32_t,
            const ::aidl::android::media::audio::common::AudioFormatDescription&) = 0;
    virtual void setAudioDevice(const ::aidl::android::media::audio::common::AudioDevice&) = 0;
    virtual void process(const void*, size_t) = 0;
};
class SoundDose final : public BnSoundDose, public StreamDataProcessorInterface {
  public:
    ndk::ScopedAStatus setOutputRs2UpperBound(float value) override;
    ndk::ScopedAStatus getOutputRs2UpperBound(float* result) override;
    ndk::ScopedAStatus registerSoundDoseCallback(
            const std::shared_ptr<ISoundDose::IHalSoundDoseCallback>& callback) override;
    void startDataProcessor(uint32_t, uint32_t,
            const ::aidl::android::media::audio::common::AudioFormatDescription&) override {}
    void setAudioDevice(const ::aidl::android::media::audio::common::AudioDevice&) override {}
    void process(const void*, size_t) override {}
  private:
    float mRs2Value = 100.0f;
};
}  // namespace aidl::android::hardware::audio::core::sounddose
