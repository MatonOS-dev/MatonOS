#pragma once

#include "core-impl/Module.h"

namespace aidl::android::hardware::audio::core {

class PipeWireModule final : public Module {
  public:
    explicit PipeWireModule(std::unique_ptr<Configuration>&& config)
        : Module(Type::DEFAULT, std::move(config)) {}

  protected:
    ndk::ScopedAStatus createInputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& metadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
            std::shared_ptr<StreamIn>* result) override;
    ndk::ScopedAStatus createOutputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& metadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offload,
            std::shared_ptr<StreamOut>* result) override;
};

}  // namespace aidl::android::hardware::audio::core
