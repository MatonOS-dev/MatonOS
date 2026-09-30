// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

#include <android/log.h>
#include <matonos_ipc.h>

#define LOG_TAG "matonos-wifid"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace fs = std::filesystem;
namespace {
constexpr char kChoice[] = "persist.vendor.maton.wifi.device";
constexpr char kPresent[] = "vendor.maton.wifi.present";
constexpr char kHwsimLoad[] = "vendor.maton.wifi.hwsim.load";

struct Device {
    std::string interface;
    std::string address;
    std::string path;
    bool simulated;
};
std::vector<Device> gDevices;
std::string gSelected;
bool gPresent = false;
MatonosIpcServer* gServer = nullptr;

std::string Read(const fs::path& path) {
    std::ifstream input(path);
    std::string value((std::istreambuf_iterator<char>(input)), {});
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}
std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c >= 0x20) out.push_back(c);
    }
    return out + "\"";
}
bool CopyJson(const std::string& json, char* result, size_t capacity) {
    if (!result || json.size() + 1 > capacity) return false;
    std::memcpy(result, json.c_str(), json.size() + 1);
    return true;
}
std::string GetProperty(const char* key) {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(key, value) > 0 ? value : "";
}
std::vector<Device> Enumerate(bool* physical) {
    std::vector<Device> devices;
    *physical = false;
    std::error_code error;
    for (fs::directory_iterator it("/sys/class/net", error), end; !error && it != end; it.increment(error)) {
        if (!fs::exists(it->path() / "phy80211", error)) { error.clear(); continue; }
        const std::string phy = fs::canonical(it->path() / "phy80211", error).string();
        error.clear();
        const std::string path = fs::canonical(it->path() / "phy80211/device", error).string();
        error.clear();
        const bool simulated = phy.find("mac80211_hwsim") != std::string::npos ||
                               path.find("mac80211_hwsim") != std::string::npos;
        if (!simulated) *physical = true;
        devices.push_back({it->path().filename().string(), Read(it->path() / "address"), path, simulated});
    }
    return devices;
}
int GetState(const char*, char* out, size_t size, void*) {
    const bool present = GetProperty(kPresent) == "1";
    return CopyJson("{\"present\":" + std::string(present ? "true" : "false") +
                    ",\"selected\":" + Quote(gSelected) + "}", out, size) ? 0 : -1;
}
int ListDevices(const char*, char* out, size_t size, void*) {
    std::string json = "{\"devices\":[";
    bool first = true;
    for (const auto& d : gDevices) {
        if (!first) json += ',';
        first = false;
        json += "{\"interface\":" + Quote(d.interface) + ",\"mac\":" + Quote(d.address) +
                ",\"device\":" + Quote(d.path) + ",\"simulated\":" +
                (d.simulated ? "true" : "false") + ",\"selected\":" +
                (d.address == gSelected || d.interface == gSelected || d.path == gSelected
                         ? "true" : "false") + "}";
    }
    return CopyJson(json + "]}", out, size) ? 0 : -1;
}
int SelectDevice(const char* args, char* out, size_t size, void*) {
    const std::string input(args ? args : "");
    const size_t key = input.find("\"device\"");
    if (key == std::string::npos) return -1;
    const size_t colon = input.find(':', key + 8);
    const size_t first = input.find('"', colon == std::string::npos ? colon : colon + 1);
    const size_t last = first == std::string::npos ? first : input.find('"', first + 1);
    if (colon == std::string::npos || first == std::string::npos || last == std::string::npos) return -1;
    const std::string choice = input.substr(first + 1, last - first - 1);
    const auto found = std::find_if(gDevices.begin(), gDevices.end(), [&](const Device& d) {
        return !d.simulated && (d.address == choice || d.interface == choice || d.path == choice);
    });
    if (found == gDevices.end() || __system_property_set(kChoice, choice.c_str()) != 0) return -1;
    gSelected = choice;
    return CopyJson("{\"selected\":" + Quote(choice) + "}", out, size) ? 0 : -1;
}
}  // namespace

int main() {
    // One bounded boot snapshot; the stock nl80211 stack owns interfaces and hotplug.
    bool physical = false;
    gDevices = Enumerate(&physical);
    if (!physical && !fs::exists("/sys/module/mac80211_hwsim")) {
        ALOGI("no physical phy80211 found; requesting one empty hwsim radio");
        if (__system_property_set(kHwsimLoad, "1") != 0) ALOGW("could not request hwsim fallback");
    }
    gSelected = GetProperty(kChoice);
    gPresent = physical || fs::exists("/sys/module/mac80211_hwsim");
    __system_property_set(kPresent, gPresent ? "1" : "0");

    gServer = matonos_ipc_create("wifi");
    if (!gServer || matonos_ipc_register(gServer, "get_state", GetState, nullptr) != 0 ||
        matonos_ipc_register(gServer, "list_devices", ListDevices, nullptr) != 0 ||
        matonos_ipc_register(gServer, "select_device", SelectDevice, nullptr) != 0 ||
        matonos_ipc_start(gServer) != 0) {
        ALOGE("cannot start Wi-Fi control channel");
        return 1;
    }
    ALOGI("Wi-Fi control channel ready; stock stack owns wlan interfaces");
    for (;;) pause();
}
