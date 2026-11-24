// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <set>
#include <string>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

#include <android/log.h>
#include <matonos_ipc.h>
#include "switch_core.h"
#include "hotplug_payload.h"

#define LOG_TAG "matonos-wifid"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace fs = std::filesystem;
namespace {
constexpr char kChoice[] = "persist.vendor.maton.wifi.device";
constexpr char kPresent[] = "vendor.maton.wifi.present";
constexpr char kHwsimLoad[] = "vendor.maton.wifi.hwsim.load";
// Debug only: lets hwsim radios stand in for real cards in switch tests.
constexpr char kHwsimSelectable[] = "persist.vendor.maton.wifi.hwsim_selectable";

struct Device {
    std::string interface;
    std::string address;
    std::string perm_mac;
    std::string path;
    bool simulated;
};
std::vector<Device> gDevices;
std::string gSelected;
bool gPresent = false;
MatonosIpcServer* gServer = nullptr;
// Binder runs handlers on several threads; one switch at a time.
std::mutex gLock;
std::set<std::string> gKnownWifiInterfaces;
std::set<std::string> gLastPhysicalMacs;

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
    std::set<std::string> seen_phys;
    *physical = false;
    std::error_code error;
    for (fs::directory_iterator it("/sys/class/net", error), end; !error && it != end; it.increment(error)) {
        if (!fs::exists(it->path() / "phy80211", error)) { error.clear(); continue; }
        const std::string phy = fs::canonical(it->path() / "phy80211", error).string();
        error.clear();
        const std::string path = fs::canonical(it->path() / "phy80211/device", error).string();
        error.clear();
        const bool simulated = GetProperty(kHwsimSelectable) != "1" &&
                               (phy.find("mac80211_hwsim") != std::string::npos ||
                                path.find("mac80211_hwsim") != std::string::npos);
        if (!seen_phys.insert(phy).second) continue;
        if (!simulated) *physical = true;
        devices.push_back({it->path().filename().string(), Read(it->path() / "address"),
                           Read(it->path() / "phy80211/macaddress"), path, simulated});
    }
    return devices;
}
std::set<std::string> PhysicalMacs(const std::vector<Device>& devices) {
    std::set<std::string> result;
    for (const auto& device : devices)
        if (!device.simulated && !device.perm_mac.empty()) result.insert(device.perm_mac);
    return result;
}
std::string Wlan0Mac(const std::vector<Device>& devices) {
    for (const auto& device : devices)
        if (device.interface == "wlan0") return device.perm_mac;
    return "";
}
void RefreshKnownNames(const std::vector<Device>& devices) {
    gKnownWifiInterfaces.clear();
    for (const auto& device : devices) gKnownWifiInterfaces.insert(device.interface);
}
void PublishDeviceChanges() {
    bool physical = false;
    gDevices = Enumerate(&physical);
    const auto current = PhysicalMacs(gDevices);
    const auto change = wifid::BuildDeviceChange(gLastPhysicalMacs, current,
                                                  GetProperty(kChoice), Wlan0Mac(gDevices));
    RefreshKnownNames(gDevices);
    if (!change.changed) return;
    gLastPhysicalMacs = current;
    gPresent = physical;
    if (__system_property_set(kPresent, gPresent ? "1" : "0") != 0)
        ALOGE("update %s after Wi-Fi hotplug failed", kPresent);
    if (!gServer || matonos_ipc_publish(gServer, "devices_changed", change.payload.c_str()) != 0)
        ALOGE("publish Wi-Fi devices_changed failed: %s", change.payload.c_str());
}
bool IsWifiEvent(const struct nlmsghdr* header) {
    if (header->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) return false;
    const auto* info = static_cast<const struct ifinfomsg*>(NLMSG_DATA(header));
    std::string name;
    int remaining = static_cast<int>(header->nlmsg_len - NLMSG_LENGTH(sizeof(*info)));
    for (auto* attr = IFLA_RTA(info); RTA_OK(attr, remaining); attr = RTA_NEXT(attr, remaining)) {
        if (attr->rta_type == IFLA_IFNAME) {
            const char* value = static_cast<const char*>(RTA_DATA(attr));
            name.assign(value, strnlen(value, RTA_PAYLOAD(attr)));
            break;
        }
    }
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lock(gLock);
    if (gKnownWifiInterfaces.count(name)) return true;
    std::error_code error;
    return fs::exists(fs::path("/sys/class/net") / name / "phy80211", error);
}
void NetlinkListener() {
    for (;;) {
        const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0) {
            const int error = errno;
            ALOGE("netlink socket failed: errno=%d (%s); retrying", error, strerror(error));
            sleep(1);
            continue;
        }
        struct sockaddr_nl address{};
        address.nl_family = AF_NETLINK;
        address.nl_groups = RTMGRP_LINK;
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
            const int error = errno;
            ALOGE("netlink bind fd=%d failed: errno=%d (%s); reopening", fd, error, strerror(error));
            ALOGE("closing netlink listener fd=%d after bind failure", fd);
            close(fd);
            sleep(1);
            continue;
        }
        bool reopen = false;
        while (!reopen) {
            char buffer[16384];
            const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count < 0) {
                const int error = errno;
                if (error == EINTR) continue;
                if (error == ENOBUFS) {
                    std::lock_guard<std::mutex> lock(gLock);
                    PublishDeviceChanges();
                    continue;
                }
                ALOGE("netlink recv fd=%d failed: errno=%d (%s); reopening", fd, error, strerror(error));
                reopen = true;
                continue;
            }
            bool relevant = false;
            int remaining = static_cast<int>(count);
            auto* header = reinterpret_cast<struct nlmsghdr*>(buffer);
            while (remaining >= static_cast<int>(sizeof(*header)) &&
                   header->nlmsg_len >= sizeof(*header) &&
                   header->nlmsg_len <= static_cast<unsigned int>(remaining)) {
                if ((header->nlmsg_type == RTM_NEWLINK || header->nlmsg_type == RTM_DELLINK) &&
                    IsWifiEvent(header)) relevant = true;
                const unsigned int step = NLMSG_ALIGN(header->nlmsg_len);
                if (step > static_cast<unsigned int>(remaining)) break;
                remaining -= static_cast<int>(step);
                header = reinterpret_cast<struct nlmsghdr*>(
                    reinterpret_cast<char*>(header) + step);
            }
            if (relevant) {
                std::lock_guard<std::mutex> lock(gLock);
                PublishDeviceChanges();
            }
        }
        ALOGE("closing netlink listener fd=%d after socket error", fd);
        close(fd);
        sleep(1);
    }
}
int GetState(const char*, char* out, size_t size, void*) {
    std::lock_guard<std::mutex> lock(gLock);
    const bool present = GetProperty(kPresent) == "1";
    return CopyJson("{\"present\":" + std::string(present ? "true" : "false") +
                    ",\"selected\":" + Quote(gSelected) + "}", out, size) ? 0 : -1;
}
int ListDevices(const char*, char* out, size_t size, void*) {
    std::lock_guard<std::mutex> lock(gLock);
    bool physical = false;
    gDevices = Enumerate(&physical);
    std::string json = "{\"devices\":[";
    bool first = true;
    for (const auto& d : gDevices) {
        if (!first) json += ',';
        first = false;
        json += "{\"interface\":" + Quote(d.interface) + ",\"mac\":" + Quote(d.address) +
                ",\"perm_mac\":" + Quote(d.perm_mac) +
                ",\"device\":" + Quote(d.path) + ",\"simulated\":" +
                (d.simulated ? "true" : "false") + ",\"selected\":" +
                (d.perm_mac == gSelected || d.interface == gSelected || d.path == gSelected
                         ? "true" : "false") + "}";
    }
    return CopyJson(json + "]}", out, size) ? 0 : -1;
}
int SelectDevice(const char* args, char* out, size_t size, void*) {
    std::lock_guard<std::mutex> lock(gLock);
    bool physical = false;
    gDevices = Enumerate(&physical);
    const std::string input(args ? args : "");
    const size_t key = input.find("\"device\"");
    if (key == std::string::npos) return -1;
    const size_t colon = input.find(':', key + 8);
    const size_t first = input.find('"', colon == std::string::npos ? colon : colon + 1);
    const size_t last = first == std::string::npos ? first : input.find('"', first + 1);
    if (colon == std::string::npos || first == std::string::npos || last == std::string::npos) return -1;
    const std::string choice = input.substr(first + 1, last - first - 1);
    const size_t persist_key = input.find("\"persist\"");
    bool persist = true;
    if (persist_key != std::string::npos) {
        const size_t persist_colon = input.find(':', persist_key + 9);
        if (persist_colon == std::string::npos) return -1;
        const size_t value = input.find_first_not_of(" \t\r\n", persist_colon + 1);
        if (value == std::string::npos) return -1;
        if (input.compare(value, 4, "true") == 0) persist = true;
        else if (input.compare(value, 5, "false") == 0) persist = false;
        else return -1;
    }
    const auto found = std::find_if(gDevices.begin(), gDevices.end(), [&](const Device& d) {
        return !d.simulated && (d.perm_mac == choice || d.interface == choice || d.path == choice);
    });
    if (found == gDevices.end()) return -1;
    const Device selected = *found;
    std::vector<wifid::SwitchInterface> interfaces;
    for (const auto& device : gDevices) interfaces.push_back({device.interface, device.simulated});
    std::string failure;
    if (wifid::SwitchTo(interfaces, selected.interface, &failure) != 0) {
        ALOGE("Wi-Fi switch to %s (%s) failed: %s", selected.interface.c_str(),
              selected.perm_mac.c_str(), failure.c_str());
        return -1;
    }
    if (persist && __system_property_set(kChoice, selected.perm_mac.c_str()) != 0) {
        ALOGE("persist selected Wi-Fi card %s failed", selected.perm_mac.c_str());
        return -1;
    }
    if (persist) gSelected = selected.perm_mac;
    return CopyJson("{\"selected\":" + Quote(selected.perm_mac) +
                    ",\"interface\":\"wlan0\"}", out, size) ? 0 : -1;
}
}  // namespace

int main() {
    // Enumerate current phy devices before exposing the control channel.
    bool physical = false;
    gDevices = Enumerate(&physical);
    gLastPhysicalMacs = PhysicalMacs(gDevices);
    RefreshKnownNames(gDevices);
    if (!physical && !fs::exists("/sys/module/mac80211_hwsim")) {
        ALOGI("no physical phy80211 found; requesting one empty hwsim radio");
        if (__system_property_set(kHwsimLoad, "1") != 0) ALOGW("could not request hwsim fallback");
    }
    gSelected = GetProperty(kChoice);
    const auto persisted = std::find_if(gDevices.begin(), gDevices.end(), [&](const Device& d) {
        return !d.simulated && d.perm_mac == gSelected;
    });
    if (persisted != gDevices.end() && persisted->interface != "wlan0") {
        std::vector<wifid::SwitchInterface> interfaces;
        for (const auto& device : gDevices) interfaces.push_back({device.interface, device.simulated});
        std::string failure;
        if (wifid::SwitchTo(interfaces, persisted->interface, &failure) != 0)
            ALOGE("boot Wi-Fi switch to %s (%s) failed: %s", persisted->interface.c_str(),
                  persisted->perm_mac.c_str(), failure.c_str());
        else ALOGI("boot Wi-Fi switch selected %s as wlan0", persisted->perm_mac.c_str());
    }
    // hwsim only keeps the stock Wi-Fi stack usable for empty scans. It is not
    // physical hardware and must not make hotspot controls appear available.
    gPresent = physical;
    __system_property_set(kPresent, gPresent ? "1" : "0");

    gServer = matonos_ipc_create("wifi");
    if (!gServer || matonos_ipc_register(gServer, "get_state", GetState, nullptr) != 0 ||
        matonos_ipc_register(gServer, "list_devices", ListDevices, nullptr) != 0 ||
        matonos_ipc_register(gServer, "select_device", SelectDevice, nullptr) != 0 ||
        matonos_ipc_start(gServer) != 0) {
        ALOGE("cannot start Wi-Fi control channel");
        return 1;
    }
    std::thread(NetlinkListener).detach();
    ALOGI("Wi-Fi control channel ready; stock stack owns wlan interfaces");
    for (;;) pause();
}
