#define LOG_TAG "AHAL_ModulePipeWire"

#include <Log.h>

#include "ModulePipeWire.h"
#include "StreamPipeWire.h"
#include "AudioControlChannel.h"
#include "core-impl/Configuration.h"

#include <spa/utils/dict.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>
#include <ctime>
#include <vector>

namespace aidl::android::hardware::audio::core {
namespace {
constexpr char kOutputTemplate[] = "PipeWire Output";
constexpr char kInputTemplate[] = "PipeWire Input";

// Framework device addresses are limited to AUDIO_DEVICE_MAX_ADDRESS_LEN (32
// incl. NUL); longer ones fail aidl2legacy conversion and abort audioserver's
// policy manager. PipeWire node.name is often longer, so publish a stable
// 64-bit FNV-1a digest of it: "pw-" + 16 hex = 19 chars.
std::string shortIdentity(const char* nodeName) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char* c = nodeName; *c != '\0'; ++c) {
        hash ^= static_cast<unsigned char>(*c);
        hash *= 0x100000001b3ULL;
    }
    char out[20];
    snprintf(out, sizeof(out), "pw-%016llx", static_cast<unsigned long long>(hash));
    return out;
}
const char* property(const spa_dict* props, const char* key) {
    return props == nullptr ? nullptr : spa_dict_lookup(props, key);
}
}  // namespace

ModulePipeWire::ModulePipeWire(std::unique_ptr<Configuration>&& config)
    : Module(Type::DEFAULT, std::move(config)) {
    mDiscoveryLoop = pw_thread_loop_new("matonos-audio-devices", nullptr);
    if (mDiscoveryLoop == nullptr) {
        LOG(ERROR) << __func__ << ": pw_thread_loop_new failed; device hotplug disabled";
        return;
    }
    if (int result = pw_thread_loop_start(mDiscoveryLoop); result < 0) {
        LOG(ERROR) << __func__ << ": pw_thread_loop_start failed result=" << result;
        return;
    }
    mDiscoveryStarted = true;
    pw_thread_loop_lock(mDiscoveryLoop);
    mRetryTimer = pw_loop_add_timer(pw_thread_loop_get_loop(mDiscoveryLoop),
                                    &ModulePipeWire::onRetryTimer, this);
    if (mRetryTimer == nullptr) {
        LOG(ERROR) << __func__ << ": pw_loop_add_timer failed; PipeWire retry disabled";
    }
    if (!connectDiscovery()) scheduleRetry();
    pw_thread_loop_unlock(mDiscoveryLoop);
}

ModulePipeWire::~ModulePipeWire() {
    if (mDiscoveryLoop == nullptr) return;
    if (mDiscoveryStarted) pw_thread_loop_lock(mDiscoveryLoop);
    if (mRetryTimer != nullptr) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(mDiscoveryLoop), mRetryTimer);
        mRetryTimer = nullptr;
    }
    if (mRegistryListenerInstalled) {
        spa_hook_remove(&mRegistryListener);
        mRegistryListenerInstalled = false;
    }
    if (mRegistry != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(mRegistry));
        mRegistry = nullptr;
    }
    if (mCoreListenerInstalled) {
        spa_hook_remove(&mCoreListener);
        mCoreListenerInstalled = false;
    }
    if (mDiscoveryCore != nullptr) {
        pw_core_disconnect(mDiscoveryCore);
        mDiscoveryCore = nullptr;
    }
    if (mDiscoveryContext != nullptr) {
        pw_context_destroy(mDiscoveryContext);
        mDiscoveryContext = nullptr;
    }
    if (mDiscoveryStarted) {
        pw_thread_loop_unlock(mDiscoveryLoop);
        pw_thread_loop_stop(mDiscoveryLoop);
    }
    pw_thread_loop_destroy(mDiscoveryLoop);
    mDiscoveryLoop = nullptr;
}

bool ModulePipeWire::connectDiscovery() {
    if (mDiscoveryContext == nullptr) {
        mDiscoveryContext = pw_context_new(pw_thread_loop_get_loop(mDiscoveryLoop), nullptr, 0);
        if (mDiscoveryContext == nullptr) {
            LOG(ERROR) << __func__ << ": pw_context_new failed";
            return false;
        }
    }
    mDiscoveryCore = pw_context_connect(mDiscoveryContext, nullptr, 0);
    if (mDiscoveryCore == nullptr) {
        PLOG(ERROR) << __func__ << ": PipeWire discovery connection unavailable";
        return false;
    }
    static const pw_core_events coreEvents = [] {
        pw_core_events value{};
        value.version = PW_VERSION_CORE_EVENTS;
        value.error = &ModulePipeWire::onCoreError;
        return value;
    }();
    pw_core_add_listener(mDiscoveryCore, &mCoreListener, &coreEvents, this);
    mCoreListenerInstalled = true;

    mRegistry = pw_core_get_registry(mDiscoveryCore, PW_VERSION_REGISTRY, 0);
    if (mRegistry == nullptr) {
        LOG(ERROR) << __func__ << ": pw_core_get_registry failed";
        return false;
    }
    static const pw_registry_events registryEvents = [] {
        pw_registry_events value{};
        value.version = PW_VERSION_REGISTRY_EVENTS;
        value.global = &ModulePipeWire::onRegistryGlobal;
        value.global_remove = &ModulePipeWire::onRegistryGlobalRemove;
        return value;
    }();
    pw_registry_add_listener(mRegistry, &mRegistryListener, &registryEvents, this);
    mRegistryListenerInstalled = true;
    mDiscoveryConnected = true;
    LOG(INFO) << __func__ << ": connected to PipeWire discovery";
    return true;
}

void ModulePipeWire::scheduleRetry() {
    if (mRetryTimer == nullptr) return;
    timespec retryIn{1, 0};  // pw_loop_update_timer takes a non-const pointer
    const int result = pw_loop_update_timer(pw_thread_loop_get_loop(mDiscoveryLoop), mRetryTimer,
                                            &retryIn, nullptr, false);
    if (result < 0) {
        LOG(ERROR) << __func__ << ": pw_loop_update_timer failed result=" << result;
    }
}

void ModulePipeWire::onRetryTimer(void* userData, uint64_t) {
    auto* self = static_cast<ModulePipeWire*>(userData);
    if (self->mDiscoveryConnected) return;
    if (self->mRegistryListenerInstalled) {
        spa_hook_remove(&self->mRegistryListener);
        self->mRegistryListenerInstalled = false;
    }
    if (self->mRegistry != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(self->mRegistry));
        self->mRegistry = nullptr;
    }
    if (self->mCoreListenerInstalled) {
        spa_hook_remove(&self->mCoreListener);
        self->mCoreListenerInstalled = false;
    }
    if (self->mDiscoveryCore != nullptr) {
        pw_core_disconnect(self->mDiscoveryCore);
        self->mDiscoveryCore = nullptr;
    }
    if (self->mDiscoveryContext != nullptr) {
        pw_context_destroy(self->mDiscoveryContext);
        self->mDiscoveryContext = nullptr;
    }
    if (!self->connectDiscovery()) self->scheduleRetry();
}

void ModulePipeWire::onCoreError(void* userData, uint32_t id, int, int result,
                                  const char* message) {
    auto* self = static_cast<ModulePipeWire*>(userData);
    if (id != PW_ID_CORE || result != -EPIPE) {
        LOG(ERROR) << __func__ << ": PipeWire core error id=" << id << " result=" << result
                   << " detail=" << (message == nullptr ? "unknown" : message);
        return;
    }
    if (!self->mDiscoveryConnected) return;
    LOG(ERROR) << __func__ << ": PipeWire discovery disconnected result=" << result
               << " detail=" << (message == nullptr ? "unknown" : message);
    self->mDiscoveryConnected = false;
    self->dropDiscoveryNodes();
    self->scheduleRetry();
}

void ModulePipeWire::dropDiscoveryNodes() {
    std::vector<uint32_t> ids;
    {
        std::lock_guard lock(mDeviceMutex);
        ids.reserve(mNodes.size());
        for (const auto& [id, node] : mNodes) ids.push_back(id);
    }
    for (uint32_t id : ids) onRegistryGlobalRemove(this, id);
}

void ModulePipeWire::onRegistryGlobal(void* userData, uint32_t id, uint32_t,
                                      const char* type, uint32_t, const spa_dict* props) {
    if (type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    static_cast<ModulePipeWire*>(userData)->handleNode(id, props);
}

void ModulePipeWire::onRegistryGlobalRemove(void* userData, uint32_t id) {
    auto* self = static_cast<ModulePipeWire*>(userData);
    bool input = false;
    std::string identity;
    {
        std::lock_guard lock(self->mDeviceMutex);
        auto it = self->mNodes.find(id);
        if (it == self->mNodes.end()) return;
        input = it->second.input;
        identity = it->second.identity;
        self->mNodes.erase(it);
    }
    setPipeWireDevice(id, identity, "", false, input);
}

void ModulePipeWire::handleNode(uint32_t id, const spa_dict* props) {
    const char* mediaClass = property(props, PW_KEY_MEDIA_CLASS);
    const bool input = mediaClass != nullptr && std::strcmp(mediaClass, "Audio/Source") == 0;
    if (mediaClass == nullptr || (!input && std::strcmp(mediaClass, "Audio/Sink") != 0)) return;
    const char* nodeName = property(props, PW_KEY_NODE_NAME);
    if (nodeName == nullptr || *nodeName == '\0') {
        LOG(ERROR) << __func__ << ": PipeWire node has no stable node.name id=" << id;
        return;
    }
    if (std::strcmp(nodeName, "maton-fallback") == 0) return;
    const char* desc = property(props, PW_KEY_NODE_DESCRIPTION);
    const char* shownName = desc != nullptr && *desc != '\0' ? desc : nodeName;
    const std::string name = shownName;
    // A digest of node.name is the framework-visible stable address. PipeWire
    // global IDs remain private to the node table and are never published.
    const std::string identity = shortIdentity(nodeName);
    LOG(INFO) << __func__ << ": node id=" << id << " node.name=" << nodeName
              << " address=" << identity;
    DeviceNode node{id, identity, name, "", input};
    node.portName = input ? kInputTemplate : kOutputTemplate;
    {
        std::lock_guard lock(mDeviceMutex);
        if (mNodes.find(id) != mNodes.end()) return;
        mNodes[id] = node;
    }
    // AudioService is the sole owner of external-device connect/disconnect.
    // The event lets the system bridge report this port through AudioManager.
    setPipeWireDevice(id, identity, name, true, input);
}


ndk::ScopedAStatus ModulePipeWire::populateConnectedDevicePort(
        ::aidl::android::media::audio::common::AudioPort* audioPort, int32_t nextPortId) {
    // The base implementation rejects every external device (it expects the
    // module to probe hardware). PipeWire device ports carry the template's
    // static profiles and PipeWire converts formats, so only the port name
    // needs filling in.
    (void)nextPortId;
    if (audioPort->ext.getTag() !=
        ::aidl::android::media::audio::common::AudioPortExt::Tag::device) {
        LOG(ERROR) << __func__ << ": not a device port: " << audioPort->toString();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& device = audioPort->ext.get<
            ::aidl::android::media::audio::common::AudioPortExt::Tag::device>().device;
    if (device.address.getTag() !=
        ::aidl::android::media::audio::common::AudioDeviceAddress::Tag::id) {
        LOG(ERROR) << __func__ << ": connected PipeWire port has non-id address";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const std::string& identity = device.address.get<
            ::aidl::android::media::audio::common::AudioDeviceAddress::Tag::id>();
    const bool input = audioPort->name == kInputTemplate;
    audioPort->name = getPipeWireDeviceName(identity, input);
    if (audioPort->name.empty()) {
        LOG(ERROR) << __func__ << ": no current PipeWire node for address=" << identity;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ndk::ScopedAStatus::ok();
}

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
