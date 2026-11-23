#define LOG_TAG "AHAL_ModulePipeWire"

#include <Log.h>

#include "ModulePipeWire.h"
#include "StreamPipeWire.h"
#include "AudioControlChannel.h"
#include "core-impl/Configuration.h"

#include <spa/utils/dict.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

namespace aidl::android::hardware::audio::core {
namespace {
constexpr char kOutputTemplate[] = "PipeWire Output";
constexpr char kInputTemplate[] = "PipeWire Input";
constexpr char kFallbackOutputTemplate[] = "No audio device";
constexpr char kFallbackInputTemplate[] = "No audio device input";

const char* property(const spa_dict* props, const char* key) {
    return props == nullptr ? nullptr : spa_dict_lookup(props, key);
}
}  // namespace

ModulePipeWire::ModulePipeWire(std::unique_ptr<Configuration>&& config)
    : Module(Type::DEFAULT, std::move(config)) {
    // Stand-ins are connected first so missing PipeWire discovery never leaves
    // AudioPolicy without a routable primary endpoint.
    syncNoDevicePorts(false);
    syncNoDevicePorts(true);
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
    std::lock_guard lock(self->mDeviceMutex);
    auto it = self->mNodes.find(id);
    if (it == self->mNodes.end()) return;
    const bool input = it->second.input;
    const std::string identity = it->second.identity;
    if (auto port = self->mConnectedPorts.find(id); port != self->mConnectedPorts.end()) {
        const int32_t portId = port->second;
        self->mConnectedPorts.erase(port);
        const auto prepareStatus = self->prepareToDisconnectExternalDevice(portId);
        if (!prepareStatus.isOk()) LOG(ERROR) << __func__ << ": prepare disconnect failed node=" << id
                                              << " port=" << portId << " status=" << prepareStatus.getDescription();
        const auto status = self->disconnectExternalDevice(portId);
        if (!status.isOk()) LOG(ERROR) << __func__ << ": disconnect failed node=" << id
                                       << " port=" << portId << " status=" << status.getDescription();
    }
    self->mNodes.erase(it);
    setPipeWireDevice(id, identity, "", false, input);
    self->syncNoDevicePorts(input);
}

void ModulePipeWire::handleNode(uint32_t id, const spa_dict* props) {
    const char* mediaClass = property(props, PW_KEY_MEDIA_CLASS);
    const bool input = mediaClass != nullptr && std::strcmp(mediaClass, "Audio/Source") == 0;
    if (mediaClass == nullptr || (!input && std::strcmp(mediaClass, "Audio/Sink") != 0)) return;
    const char* nodeName = property(props, PW_KEY_NODE_NAME);
    if (nodeName != nullptr && std::strcmp(nodeName, "maton-fallback") == 0) return;
    const char* desc = property(props, PW_KEY_NODE_DESCRIPTION);
    const char* shownName = desc != nullptr && *desc != '\0' ? desc : nodeName;
    const std::string name = shownName != nullptr ? shownName : "PipeWire audio device";
    const std::string identity = makePipeWireIdentity(property(props, PW_KEY_DEVICE_BUS_PATH),
            property(props, PW_KEY_DEVICE_SERIAL), property(props, PW_KEY_DEVICE_NAME),
            property(props, "device.profile.name"), nodeName, input);
    DeviceNode node{id, identity, name, "", input, false};
    node.portName = input ? kInputTemplate : kOutputTemplate;
    {
        std::lock_guard lock(mDeviceMutex);
        if (mNodes.find(id) != mNodes.end()) return;
        mNodes[id] = node;
    }
    auto templateIt = std::find_if(getConfig().ports.begin(), getConfig().ports.end(),
            [&](const auto& port) { return port.name == node.portName; });
    if (templateIt == getConfig().ports.end()) {
        LOG(ERROR) << __func__ << ": missing device template=" << node.portName;
        return;
    }
    ::aidl::android::media::audio::common::AudioPort request = *templateIt;
    auto& device = request.ext.get<::aidl::android::media::audio::common::AudioPortExt::Tag::device>().device;
    device.address.set<::aidl::android::media::audio::common::AudioDeviceAddress::id>(identity);
    mPendingPort = node;
    ::aidl::android::media::audio::common::AudioPort connected;
    const auto status = connectExternalDevice(request, &connected);
    if (!status.isOk()) {
        LOG(ERROR) << __func__ << ": connectExternalDevice failed node=" << id
                   << " identity=" << identity << " status=" << status.getDescription();
        {
            std::lock_guard lock(mDeviceMutex);
            mNodes.erase(id);
        }
        return;
    }
    {
        std::lock_guard lock(mDeviceMutex);
        mConnectedPorts[id] = connected.id;
    }
    setPipeWireDevice(id, identity, name, true, input);
    syncNoDevicePorts(input);
}

void ModulePipeWire::syncNoDevicePorts(bool input) {
    const bool anyDevice = std::any_of(mNodes.begin(), mNodes.end(),
            [input](const auto& entry) { return entry.second.input == input; });
    const auto templateName = input ? kFallbackInputTemplate : kFallbackOutputTemplate;
    if (anyDevice && mFallbackPorts.find(input) != mFallbackPorts.end()) {
        const int32_t portId = mFallbackPorts[input];
        const auto prepareStatus = prepareToDisconnectExternalDevice(portId);
        if (!prepareStatus.isOk()) {
            LOG(ERROR) << __func__ << ": fallback prepare disconnect failed input=" << input
                       << " port=" << portId << " status=" << prepareStatus.getDescription();
            return;
        }
        const auto status = disconnectExternalDevice(portId);
        if (!status.isOk()) {
            LOG(ERROR) << __func__ << ": fallback disconnect failed input=" << input
                       << " port=" << portId << " status=" << status.getDescription();
            return;
        }
        mFallbackPorts.erase(input);
    } else if (!anyDevice && mFallbackPorts.find(input) == mFallbackPorts.end()) {
        auto templateIt = std::find_if(getConfig().ports.begin(), getConfig().ports.end(),
                [&](const auto& port) { return port.name == templateName; });
        if (templateIt == getConfig().ports.end()) {
            LOG(ERROR) << __func__ << ": missing fallback template=" << templateName;
            return;
        }
        auto request = *templateIt;
        auto& device = request.ext.get<::aidl::android::media::audio::common::AudioPortExt::Tag::device>().device;
        const std::string identity = input ? "fallback-input" : "fallback-output";
        device.address.set<::aidl::android::media::audio::common::AudioDeviceAddress::id>(identity);
        mPendingPort = DeviceNode{PW_ID_ANY, identity, "No audio device", templateName, input, true};
        ::aidl::android::media::audio::common::AudioPort connected;
        const auto status = connectExternalDevice(request, &connected);
        if (!status.isOk()) {
            LOG(ERROR) << __func__ << ": fallback connect failed input=" << input
                       << " status=" << status.getDescription();
            return;
        }
        mFallbackPorts[input] = connected.id;
    }
}


ndk::ScopedAStatus ModulePipeWire::populateConnectedDevicePort(
        ::aidl::android::media::audio::common::AudioPort* audioPort, int32_t nextPortId) {
    auto status = Module::populateConnectedDevicePort(audioPort, nextPortId);
    if (!status.isOk()) return status;
    audioPort->name = mPendingPort.name;
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
