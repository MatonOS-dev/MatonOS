#include "MatonConfig.h"

namespace aidl::android::hardware::audio::core {

ndk::ScopedAStatus MatonConfig::getSurroundSoundConfig(SurroundSoundConfig* result) {
    *result = SurroundSoundConfig{};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus MatonConfig::getEngineConfig(
        ::aidl::android::media::audio::common::AudioHalEngineConfig* result) {
    // The AOSP example HAL returns a successful empty config when its optional
    // XML converter has no valid engine config. AudioFlinger then gathers our
    // live AIDL IModule ports and routes instead of trying to reconstruct the
    // primary output from the legacy XML-only path. The framework supplies its
    // built-in default product strategies and volume groups for this case.
    *result = {};
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
