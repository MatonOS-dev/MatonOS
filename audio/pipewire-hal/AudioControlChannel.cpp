#define LOG_TAG "AHAL_AudioControlChannel"

#include <cstdlib>
#include "AudioControlChannel.h"
#include "../network-speakers/NetworkSpeakers.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

#include <Log.h>
#include <android-base/file.h>

namespace aidl::android::hardware::audio::core {
namespace {
constexpr char kBoostFile[] = "/data/vendor/maton-audio/device-boosts.conf";
struct Device {
    uint32_t nodeId;
    std::string identity;
    std::string name;
    bool input;
};
std::mutex gMutex;
std::map<std::string, Device> gDevices;
std::map<std::string, float> gBoosts;
std::vector<std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>> gListeners;

const char* deviceType(bool input) { return input ? "in" : "out"; }

void loadBoostsLocked() {
    std::ifstream input(kBoostFile);
    std::string identity;
    float boost = 1.0f;
    while (input >> identity >> boost) {
        if (!identity.empty() && std::isfinite(boost)) {
            gBoosts[identity] = std::clamp(boost, 0.5f, 1.5f);
        }
    }
    if (input.bad()) PLOG(ERROR) << "failed reading " << kBoostFile;
}

bool saveBoostsLocked() {
    std::ostringstream contents;
    for (const auto& [identity, boost] : gBoosts) contents << identity << ' ' << boost << '\n';
    if (!::android::base::WriteStringToFile(contents.str(), kBoostFile)) {
        PLOG(ERROR) << "failed writing " << kBoostFile;
        return false;
    }
    return true;
}

std::string escapeJson(std::string_view value) {
    std::string result;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
            result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

bool jsonString(const std::string& json, const char* key, std::string* value) {
    const std::regex expression(std::string("\\\"") + key +
            "\\\"\\s*:\\s*\\\"([A-Za-z0-9_.:/-]+)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return false;
    *value = match[1].str();
    return true;
}

bool jsonNumber(const std::string& json, const char* key, float* value) {
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return false;
    // The regex admits only a plain decimal, so strtof cannot fail to parse.
    *value = std::strtof(match[1].str().c_str(), nullptr);
    return std::isfinite(*value);
}
}  // namespace

void setPipeWireDevice(uint32_t nodeId, const std::string& identity, const std::string& name,
                       bool connected, bool input) {
    std::vector<std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>> listeners;
    std::string description = name;
    std::string event;
    {
        std::lock_guard lock(gMutex);
        if (connected) {
            gDevices[identity] = Device{nodeId, identity, name, input};
        } else {
            const auto it = gDevices.find(identity);
            if (it == gDevices.end()) return;
            description = it->second.name;
            gDevices.erase(it);
        }
        event = std::string("{\"event\":\"") + (connected ? "add" : "remove") +
                "\",\"device\":{\"type\":\"" + deviceType(input) +
                "\",\"address\":\"" + escapeJson(identity) + "\",\"description\":\"" +
                escapeJson(description) + "\"}}";
        listeners = gListeners;
    }
    std::vector<std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>> failed;
    for (const auto& listener : listeners) {
        const auto status = listener->onEvent("devices", event);
        if (!status.isOk()) {
            LOG(ERROR) << __func__ << ": event delivery failed address=" << identity
                       << " type=" << deviceType(input)
                       << " status=" << status.getDescription();
            failed.push_back(listener);
        }
    }
    if (!failed.empty()) {
        std::lock_guard lock(gMutex);
        gListeners.erase(std::remove_if(gListeners.begin(), gListeners.end(),
                [&](const auto& listener) {
                    return std::find(failed.begin(), failed.end(), listener) != failed.end();
                }), gListeners.end());
    }
}

std::string getPipeWireDeviceName(const std::string& identity, bool input) {
    std::lock_guard lock(gMutex);
    const auto it = gDevices.find(identity);
    return it == gDevices.end() || it->second.input != input ? "" : it->second.name;
}

float getPipeWireDeviceBoost(const std::string& identity) {
    std::lock_guard lock(gMutex);
    if (gBoosts.empty()) loadBoostsLocked();
    const auto it = gBoosts.find(identity);
    return it == gBoosts.end() ? 1.0f : it->second;
}

std::string pipeWireIdentity(
        const ::aidl::android::media::audio::common::AudioDeviceAddress& address) {
    using Tag = ::aidl::android::media::audio::common::AudioDeviceAddress::Tag;
    if (address.getTag() == Tag::id) return address.get<Tag::id>();
    if (address.getTag() != Tag::ipv4) return "";
    const auto& bytes = address.get<Tag::ipv4>();
    if (bytes.size() != 4) {
        LOG(ERROR) << __func__ << ": ipv4 address with " << bytes.size() << " bytes";
        return "";
    }
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
           std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

uint32_t getPipeWireTargetNode(const std::string& identity) {
    std::lock_guard lock(gMutex);
    const auto it = gDevices.find(identity);
    return it == gDevices.end() ? std::numeric_limits<uint32_t>::max() : it->second.nodeId;
}

ndk::ScopedAStatus AudioControlChannel::call(const std::string& command,
                                             const std::string& jsonArgs,
                                             std::string* result) {
    if (command == "network_upsert" || command == "network_remove" || command == "network_reset") {
        const int status = maton_network_speaker(command.c_str(), jsonArgs.c_str());
        if (status < 0) {
            LOG(ERROR) << "network speaker command=" << command << " failed result=" << status;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        *result = "{\"queued\":true}";
        return ndk::ScopedAStatus::ok();
    }
    std::lock_guard lock(gMutex);
    if (gBoosts.empty()) loadBoostsLocked();
    if (command == "devices" || command == "get_devices") {
        std::ostringstream json;
        json << "{\"devices\":[";
        bool first = true;
        for (const auto& [id, device] : gDevices) {
            if (command == "get_devices" && device.input) continue;
            if (!first) json << ',';
            first = false;
            json << "{\"type\":\"" << deviceType(device.input) << "\",\"address\":\""
                 << escapeJson(id) << "\",\"description\":\""
                 << escapeJson(device.name) << '\"';
            if (command == "get_devices") {
                const auto boost = gBoosts.find(id);
                json << ",\"id\":\"" << escapeJson(id) << "\",\"name\":\""
                     << escapeJson(device.name) << "\",\"multiplier\":"
                     << (boost == gBoosts.end() ? 1.0f : boost->second);
            }
            json << '}';
        }
        json << "]}";
        *result = json.str();
        return ndk::ScopedAStatus::ok();
    }
    if (command == "set_boost") {
        std::string identity;
        float boost = 1.0f;
        if (!jsonString(jsonArgs, "id", &identity) || !jsonNumber(jsonArgs, "multiplier", &boost) ||
            boost < 0.5f || boost > 1.5f || gDevices.find(identity) == gDevices.end()) {
            LOG(ERROR) << __func__ << ": invalid audio boost request";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        gBoosts[identity] = boost;
        if (!saveBoostsLocked()) return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
        *result = "{\"saved\":true}";
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": unsupported command=" << command;
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus AudioControlChannel::subscribe(
        const std::string& topic,
        const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>& listener) {
    if (topic != "devices" || listener == nullptr) {
        LOG(ERROR) << __func__ << ": invalid audio subscription topic=" << topic;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(gMutex);
    if (std::find(gListeners.begin(), gListeners.end(), listener) == gListeners.end()) {
        gListeners.push_back(listener);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AudioControlChannel::unsubscribe(
        const std::string& topic,
        const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>& listener) {
    if (topic != "devices") {
        LOG(ERROR) << __func__ << ": unknown audio topic=" << topic;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(gMutex);
    gListeners.erase(std::remove(gListeners.begin(), gListeners.end(), listener), gListeners.end());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AudioControlChannel::stageUpdate(
        const std::vector<std::string>&, const std::vector<ndk::ScopedFileDescriptor>&,
        std::string*) {
    LOG(ERROR) << __func__ << ": update operation is not supported on the audio channel";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus AudioControlChannel::finalizeUpdate(std::string*) {
    LOG(ERROR) << __func__ << ": update operation is not supported on the audio channel";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus AudioControlChannel::getUpdateStatus(std::string*) {
    LOG(ERROR) << __func__ << ": update operation is not supported on the audio channel";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::audio::core

