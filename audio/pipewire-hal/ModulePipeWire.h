#pragma once

#include "core-impl/Module.h"

namespace aidl::android::hardware::audio::core {

class ModulePipeWire final : public Module {
  public:
    explicit ModulePipeWire(std::unique_ptr<Configuration>&& config)
        : Module(Type::DEFAULT, std::move(config)) {}

  protected:
    ndk::ScopedAStatus createInputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
            std::shared_ptr<StreamIn>* result) override;
    ndk::ScopedAStatus createOutputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offloadInfo,
            std::shared_ptr<StreamOut>* result) override;
    std::unique_ptr<Configuration> initializeConfig() override;
};

}  // namespace aidl::android::hardware::audio::core
