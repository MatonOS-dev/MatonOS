#pragma once

#include <map>
#include <mutex>
#include <string>

#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>

#include "core-impl/Module.h"

namespace aidl::android::hardware::audio::core {

class ModulePipeWire final : public Module {
  public:
    explicit ModulePipeWire(std::unique_ptr<Configuration>&& config);
    ~ModulePipeWire() override;

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
    ndk::ScopedAStatus populateConnectedDevicePort(
            ::aidl::android::media::audio::common::AudioPort* audioPort,
            int32_t nextPortId) override;

  private:
    struct DeviceNode {
        uint32_t nodeId;
        std::string identity;
        std::string name;
        std::string portName;
        bool input;
        bool fallback;
    };
    static void onRegistryGlobal(void* userData, uint32_t id, uint32_t permissions,
                                 const char* type, uint32_t version, const spa_dict* props);
    static void onRegistryGlobalRemove(void* userData, uint32_t id);
    static void onCoreError(void* userData, uint32_t id, int seq, int result,
                            const char* message);
    static void onRetryTimer(void* userData, uint64_t expirations);
    void handleNode(uint32_t id, const spa_dict* props);
    void syncNoDevicePorts(bool input);
    void scheduleRetry();
    bool connectDiscovery();
    void dropDiscoveryNodes();

    pw_thread_loop* mDiscoveryLoop = nullptr;
    pw_context* mDiscoveryContext = nullptr;
    pw_core* mDiscoveryCore = nullptr;
    pw_registry* mRegistry = nullptr;
    spa_source* mRetryTimer = nullptr;
    spa_hook mCoreListener{};
    spa_hook mRegistryListener{};
    bool mDiscoveryStarted = false;
    bool mCoreListenerInstalled = false;
    bool mRegistryListenerInstalled = false;
    bool mDiscoveryConnected = false;
    std::mutex mDeviceMutex;
    std::map<uint32_t, DeviceNode> mNodes;
    std::map<uint32_t, int32_t> mConnectedPorts;
    std::map<bool, int32_t> mFallbackPorts;
    DeviceNode mPendingPort{};
};

}  // namespace aidl::android::hardware::audio::core
