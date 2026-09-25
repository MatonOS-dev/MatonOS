#pragma once

#include <aidl/android/hardware/audio/core/BnConfig.h>

namespace aidl::android::hardware::audio::core {

// The sound stack uses AOSP's vendor audio policy XML directly. The HAL only
// provides the optional engine/surround configuration interface here.
class MatonConfig final : public BnConfig {
  public:
    ndk::ScopedAStatus getSurroundSoundConfig(SurroundSoundConfig* result) override;
    ndk::ScopedAStatus getEngineConfig(
            ::aidl::android::media::audio::common::AudioHalEngineConfig* result) override;
};

}  // namespace aidl::android::hardware::audio::core
