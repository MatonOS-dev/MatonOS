#pragma once

#include <aidl/vendor/matonos/channel/BnChannel.h>
#include <android/binder_auto_utils.h>
#include <cstdint>
#include <string>
#include <vector>

namespace aidl::android::hardware::audio::core {

class AudioControlChannel final : public ::aidl::vendor::matonos::channel::BnChannel {
  public:
    ndk::ScopedAStatus call(const std::string& command, const std::string& jsonArgs,
                            std::string* result) override;
    ndk::ScopedAStatus subscribe(
            const std::string& topic,
            const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>& listener)
            override;
    ndk::ScopedAStatus unsubscribe(
            const std::string& topic,
            const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>& listener)
            override;
    ndk::ScopedAStatus stageUpdate(
            const std::vector<std::string>& names,
            const std::vector<ndk::ScopedFileDescriptor>& files,
            std::string* result) override;
    ndk::ScopedAStatus finalizeUpdate(std::string* result) override;
    ndk::ScopedAStatus getUpdateStatus(std::string* result) override;
};

void setPipeWireDevice(uint32_t nodeId, const std::string& identity, const std::string& name,
                       bool connected, bool input);
float getPipeWireDeviceBoost(const std::string& identity);
uint32_t getPipeWireTargetNode(const std::string& identity);
std::string makePipeWireIdentity(const char* busPath, const char* serial, const char* deviceName,
                                 const char* profileName, const char* nodeName, bool input);

}  // namespace aidl::android::hardware::audio::core
