#define LOG_TAG "AHAL_ModulePipeWire"

#include <Log.h>

#include "ModulePipeWire.h"
#include "StreamPipeWire.h"
#include "core-impl/Configuration.h"

namespace aidl::android::hardware::audio::core {

std::unique_ptr<Module::Configuration> ModulePipeWire::initializeConfig() {
    // The normal service supplies its policy-converted config. Keep AOSP's
    // primary config as a safe fallback if the service is instantiated alone.
    return internal::getConfiguration(Type::DEFAULT);
}

ndk::ScopedAStatus ModulePipeWire::createInputStream(
        StreamContext&& context,
        const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
        const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
        std::shared_ptr<StreamIn>* result) {
    if (context.isMmap() || context.isOffload()) {
        LOG(ERROR) << __func__ << ": PipeWire primary does not support mmap/offload input";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return createStreamInstance<StreamInPipeWire>(result, std::move(context), sinkMetadata,
                                                  microphones);
}

ndk::ScopedAStatus ModulePipeWire::createOutputStream(
        StreamContext&& context,
        const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
        const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offloadInfo,
        std::shared_ptr<StreamOut>* result) {
    if (context.isMmap() || context.isOffload() || offloadInfo.has_value()) {
        LOG(ERROR) << __func__ << ": PipeWire primary does not support mmap/offload output";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return createStreamInstance<StreamOutPipeWire>(result, std::move(context), sourceMetadata,
                                                   offloadInfo);
}

}  // namespace aidl::android::hardware::audio::core
