#define LOG_TAG "AHAL_AudioControlChannel"

#include "AudioControlChannel.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>

#include <Log.h>
#include <android-base/file.h>

namespace aidl::android::hardware::audio::core {
namespace {
constexpr char kBoostFile[] = "/data/vendor/maton-audio/device-boosts.conf";
struct Device {
    uint32_t nodeId;
    std::string identity;
    std::string name;
    bool connected;
    bool input;
};
std::mutex gMutex;
std::map<std::string, Device> gDevices;
std::map<std::string, float> gBoosts;

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
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([a-fA-F0-9]+)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return false;
    *value = match[1].str();
    return true;
}

bool jsonNumber(const std::string& json, const char* key, float* value) {
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return false;
    try {
        *value = std::stof(match[1].str());
        return std::isfinite(*value);
    } catch (const std::exception&) {
        return false;
    }
}
}  // namespace

void setPipeWireDevice(uint32_t nodeId, const std::string& identity, const std::string& name,
                       bool connected, bool input) {
    std::lock_guard lock(gMutex);
    if (connected) {
        gDevices[identity] = Device{nodeId, identity, name, true, input};
    } else if (auto it = gDevices.find(identity); it != gDevices.end()) {
        it->second.connected = false;
    }
}

float getPipeWireDeviceBoost(const std::string& identity) {
    std::lock_guard lock(gMutex);
    if (gBoosts.empty()) loadBoostsLocked();
    const auto it = gBoosts.find(identity);
    return it == gBoosts.end() ? 1.0f : it->second;
}

uint32_t getPipeWireTargetNode(const std::string& identity) {
    std::lock_guard lock(gMutex);
    const auto it = gDevices.find(identity);
    return it == gDevices.end() || !it->second.connected ? std::numeric_limits<uint32_t>::max() : it->second.nodeId;
}

std::string makePipeWireIdentity(const char* busPath, const char* serial, const char* deviceName,
                                 const char* profileName, const char* nodeName, bool input) {
    // Device identity deliberately excludes PipeWire's transient global node ID and ALSA card.
    std::string stable = busPath != nullptr && *busPath != '\0' ? busPath
            : serial != nullptr && *serial != '\0' ? serial
            : deviceName != nullptr && *deviceName != '\0' ? deviceName
            : nodeName != nullptr ? nodeName : "unknown";
    stable += "|";
    stable += profileName != nullptr ? profileName : "";
    stable += "|";
    stable += nodeName != nullptr ? nodeName : "";
    stable += input ? "|input" : "|output";
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : stable) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char key[17];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    return key;
}

ndk::ScopedAStatus AudioControlChannel::call(const std::string& command,
                                             const std::string& jsonArgs,
                                             std::string* result) {
    std::lock_guard lock(gMutex);
    if (gBoosts.empty()) loadBoostsLocked();
    if (command == "get_devices") {
        std::ostringstream json;
        json << "{\"devices\":[";
        bool first = true;
        for (const auto& [id, device] : gDevices) {
            if (!device.connected || device.input) continue;
            if (!first) json << ',';
            first = false;
            const auto boost = gBoosts.find(id);
            json << "{\"id\":\"" << escapeJson(id) << "\",\"name\":\""
                 << escapeJson(device.name) << "\",\"multiplier\":"
                 << (boost == gBoosts.end() ? 1.0f : boost->second) << '}';
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
        const std::string&, const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>&) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AudioControlChannel::unsubscribe(
        const std::string&, const std::shared_ptr<::aidl::vendor::matonos::channel::IChannelListener>&) {
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

