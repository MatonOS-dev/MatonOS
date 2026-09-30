// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <sys/system_properties.h>
#include <thread>
#include <vector>

#include <android/log.h>
#include <matonos_ipc.h>

#define LOG_TAG "matonos-wifid"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr char kSysNet[] = "/sys/class/net";
constexpr char kSysRfkill[] = "/sys/class/rfkill";
constexpr char kPersistChoice[] = "persist.vendor.maton.wifi.device";
constexpr char kPresentProp[] = "vendor.maton.wifi.present";
constexpr char kSelectedProp[] = "vendor.maton.wifi.selected";
constexpr char kHwsimLoadProp[] = "vendor.maton.wifi.hwsim.load";
constexpr char kHwsimRadiosProp[] = "vendor.maton.wifi.hwsim.radios";
constexpr char kHwsimTestReadyProp[] = "vendor.maton.wifi.hwsim.test_ready";
constexpr char kPersistHwsimRadiosProp[] = "persist.vendor.maton.wifi.hwsim_radios";
constexpr char kWifiIfname[] = "wlan0";
constexpr size_t kRfkillPrefixLength = sizeof("rfkill") - 1;
constexpr size_t kMaxDeviceArgumentLength = 512;

struct Adapter {
    std::string ifname;
    std::string mac;
    std::string device;
    std::string phy;
    bool built_in = false;
    bool simulated = false;
};

std::mutex gStateMutex;
std::vector<Adapter> gAdapters;
std::string gSelectedKey;
bool gPresent = false;
bool gTestApReady = false;
MatonosIpcServer* gIpcServer = nullptr;

std::string Read(const fs::path& path) {
    std::ifstream input(path);
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    return value.substr(first);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return Lower(a) == Lower(b);
}

void CopyIfreqName(char* destination, size_t capacity, const std::string& name) {
    if (!capacity) return;
    std::strncpy(destination, name.c_str(), capacity - 1);
    destination[capacity - 1] = '\0';
}

std::string Canonical(const fs::path& path) {
    std::error_code error;
    const auto result = fs::canonical(path, error);
    return error ? std::string() : result.string();
}

bool IsUsb(const std::string& path) {
    return path.find("/usb") != std::string::npos || path.find("/usb-") != std::string::npos;
}

std::vector<Adapter> Enumerate() {
    std::vector<Adapter> adapters;
    std::error_code error;
    fs::directory_iterator entry(kSysNet, error), end;
    while (!error && entry != end) {
        const auto current = entry;
        entry.increment(error);
        if (error) break;
        const std::string name = current->path().filename().string();
        if (!fs::exists(current->path() / "phy80211", error)) { error.clear(); continue; }
        std::string device = Canonical(current->path() / "phy80211" / "device");
        if (device.empty()) {
            const std::string phy_path = Canonical(current->path() / "phy80211");
            if (!phy_path.empty()) device = Canonical(fs::path(phy_path).parent_path() / "device");
        }
        const std::string phy_path = Canonical(current->path() / "phy80211");
        const std::string address = Read(current->path() / "address");
        const std::string key = !address.empty() ? Lower(address) : device;
        if (key.empty()) continue;

        // Several netdevs can belong to one phy (P2P/monitor). Keep one STA-like
        // candidate for Android and retain an already selected renamed netdev.
        auto existing = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) {
            return (!device.empty() && a.device == device) || a.mac == key;
        });
        const bool simulated = device.find("mac80211_hwsim") != std::string::npos ||
                               phy_path.find("mac80211_hwsim") != std::string::npos;
        Adapter candidate{name, key, device, phy_path, !device.empty() && !IsUsb(device), simulated};
        if (existing == adapters.end()) {
            adapters.push_back(std::move(candidate));
        } else if ((name.rfind("wlan", 0) == 0) || existing->ifname.rfind("wlan", 0) != 0) {
            *existing = std::move(candidate);
        }
    }
    std::sort(adapters.begin(), adapters.end(), [](const Adapter& a, const Adapter& b) {
        if (a.built_in != b.built_in) return a.built_in > b.built_in;
        if (a.simulated != b.simulated) return a.simulated < b.simulated;
        return a.device < b.device;
    });
    return adapters;
}

bool MatchesChoice(const Adapter& adapter, const std::string& choice) {
    if (choice.empty()) return false;
    return EqualsIgnoreCase(choice, adapter.mac) ||
           choice == adapter.device || choice == adapter.phy ||
           choice == fs::path(adapter.phy).filename().string() || choice == adapter.ifname;
}

std::string ReadChoice() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(kPersistChoice, value) > 0 ? value : "";
}

void SetInterfaceDown(const std::string& name) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq request {};
    CopyIfreqName(request.ifr_name, sizeof(request.ifr_name), name);
    if (ioctl(fd, SIOCGIFFLAGS, &request) == 0 && (request.ifr_flags & IFF_UP)) {
        request.ifr_flags &= ~IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &request) != 0) {
            ALOGW("cannot bring unused Wi-Fi interface %s down: %s", name.c_str(),
                  strerror(errno));
        }
    }
    close(fd);
}

bool Rename(const std::string& old_name, const std::string& new_name) {
    if (old_name == new_name) return true;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct ifreq request {};
    CopyIfreqName(request.ifr_name, sizeof(request.ifr_name), old_name);
    bool ok = ioctl(fd, SIOCGIFFLAGS, &request) == 0;
    if (ok && (request.ifr_flags & IFF_UP)) {
        request.ifr_flags &= ~IFF_UP;
        ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
    }
    if (ok) {
        CopyIfreqName(request.ifr_newname, sizeof(request.ifr_newname), new_name);
        ok = ioctl(fd, SIOCSIFNAME, &request) == 0;
    }
    const int saved_errno = errno;
    close(fd);
    if (!ok) ALOGW("cannot rename Wi-Fi interface %s to %s: %s", old_name.c_str(),
                   new_name.c_str(), strerror(saved_errno));
    return ok;
}

bool SetSoftBlock(unsigned int index, bool block) {
    const std::string base = std::string(kSysRfkill) + "/rfkill" + std::to_string(index);
    const std::string type = Read(base + "/type");
    if (type != "wlan" && type != "wifi") return true;
    const std::string current = Read(base + "/soft");
    if ((!current.empty() && (current == "1") == block)) return true;

    const int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct rfkill_event event {};
    event.idx = index;
    event.type = RFKILL_TYPE_WLAN;
    event.op = RFKILL_OP_CHANGE;
    event.soft = block ? 1 : 0;
    const ssize_t written = write(fd, &event, sizeof(event));
    const int saved_errno = errno;
    close(fd);
    if (written != sizeof(event)) {
        ALOGW("rfkill update for index %u failed: %s", index,
              written < 0 ? strerror(saved_errno) : "short write");
        return false;
    }
    return true;
}

void ApplyRfkill(const std::vector<Adapter>& adapters, const std::string& selected_key,
                 const std::string& test_ap_key = {}) {
    std::error_code error;
    fs::directory_iterator entry(kSysRfkill, error), end;
    while (!error && entry != end) {
        const auto current = entry;
        entry.increment(error);
        if (error) break;
        const std::string name = current->path().filename().string();
        if (name.rfind("rfkill", 0) != 0) continue;
        const std::string type = Read(current->path() / "type");
        if (type != "wlan" && type != "wifi") continue;

        const std::string rf_device = Canonical(current->path() / "device");
        bool belongs_to_selected = false;
        bool belongs_to_any = false;
        for (const auto& adapter : adapters) {
            const bool same = !rf_device.empty() && !adapter.device.empty() &&
                              (rf_device == adapter.device ||
                               rf_device.rfind(adapter.device + "/", 0) == 0 ||
                               adapter.device.rfind(rf_device + "/", 0) == 0);
            if (!same) continue;
            belongs_to_any = true;
            if (adapter.mac == selected_key || adapter.mac == test_ap_key) {
                belongs_to_selected = true;
            }
        }
        if (!belongs_to_any) continue;
        unsigned int index = 0;
        try {
            index = static_cast<unsigned int>(std::stoul(name.substr(kRfkillPrefixLength)));
        } catch (...) {
            continue;
        }
        // Android brings wlan0 up after it starts Wi-Fi. Blocking the chosen
        // radio while wlan0 is down prevents that transition from succeeding.
        SetSoftBlock(index, !belongs_to_selected);
    }
}

std::string TemporaryName(size_t index) {
    // Stable unique name within this enumeration; Linux itself rejects collisions.
    return "mtnw" + std::to_string(index);
}

std::string KeepOthersOut(const std::vector<Adapter>& adapters, const std::string& selected_key,
                          bool reserve_hwsim_test_radio) {
    std::string test_ap_key;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if (adapter.mac == selected_key) continue;
        // Two-radio hwsim is an opt-in QEMU test fixture. Leave its second
        // nl80211 interface available as wlan1 for a guest hostapd process.
        if (reserve_hwsim_test_radio && adapter.simulated && test_ap_key.empty()) {
            if (adapter.ifname != "wlan1" && Rename(adapter.ifname, "wlan1")) {
                test_ap_key = adapter.mac;
            } else if (adapter.ifname == "wlan1") {
                test_ap_key = adapter.mac;
            }
            continue;
        }
        if (adapter.ifname.rfind("wlan", 0) == 0) {
            if (!Rename(adapter.ifname, TemporaryName(i))) SetInterfaceDown(adapter.ifname);
        } else {
            SetInterfaceDown(adapter.ifname);
        }
    }
    return test_ap_key;
}

bool Publish(const char* key, const std::string& value) {
    if (value.size() >= PROP_VALUE_MAX) { ALOGW("property value too long for %s", key); return false; }
    char current[PROP_VALUE_MAX] = {};
    __system_property_get(key, current);
    if (value == current) return true;
    const int result = __system_property_set(key, value.c_str());
    if (result != 0) ALOGW("cannot set %s", key);
    return result == 0;
}

std::string JsonString(const std::string& value) {
    std::string escaped = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            constexpr char kHex[] = "0123456789abcdef";
            escaped += "\\u00";
            escaped.push_back(kHex[c >> 4]);
            escaped.push_back(kHex[c & 0x0f]);
        } else {
            escaped.push_back(static_cast<char>(c));
        }
    }
    escaped.push_back('"');
    return escaped;
}

void SkipWhitespace(const std::string& input, size_t* position) {
    while (*position < input.size() && std::isspace(static_cast<unsigned char>(input[*position]))) {
        ++*position;
    }
}

bool ParseDeviceArgument(const char* json, std::string* device) {
    const std::string input(json ? json : "");
    size_t position = 0;
    SkipWhitespace(input, &position);
    if (position >= input.size() || input[position++] != '{') return false;
    SkipWhitespace(input, &position);
    constexpr char kName[] = "\"device\"";
    if (input.compare(position, sizeof(kName) - 1, kName) != 0) return false;
    position += sizeof(kName) - 1;
    SkipWhitespace(input, &position);
    if (position >= input.size() || input[position++] != ':') return false;
    SkipWhitespace(input, &position);
    if (position >= input.size() || input[position++] != '"') return false;
    device->clear();
    while (position < input.size() && input[position] != '"') {
        const unsigned char c = static_cast<unsigned char>(input[position++]);
        if (c < 0x20) return false;
        if (c == '\\') {
            if (position >= input.size()) return false;
            const char escaped = input[position++];
            if (escaped != '"' && escaped != '\\' && escaped != '/') return false;
            device->push_back(escaped);
        } else {
            device->push_back(static_cast<char>(c));
        }
        if (device->size() > kMaxDeviceArgumentLength) return false;
    }
    if (position >= input.size() || input[position++] != '"') return false;
    SkipWhitespace(input, &position);
    if (position >= input.size() || input[position++] != '}') return false;
    SkipWhitespace(input, &position);
    return position == input.size() && !device->empty();
}

int HandleGetState(const char*, char* result, size_t capacity, void*) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    const std::string selected = gSelectedKey.empty()
            ? std::string()
            : (std::any_of(gAdapters.begin(), gAdapters.end(), [&](const Adapter& adapter) {
                   return adapter.mac == gSelectedKey && adapter.simulated;
               }) ? "hwsim:" + gSelectedKey : gSelectedKey);
    const std::string json = "{\"present\":" + std::string(gPresent ? "true" : "false") +
            ",\"selected\":" + JsonString(selected) +
            ",\"testApReady\":" + (gTestApReady ? "true" : "false") + "}";
    if (json.size() + 1 > capacity) return -1;
    std::memcpy(result, json.c_str(), json.size() + 1);
    return 0;
}

int HandleListDevices(const char*, char* result, size_t capacity, void*) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    std::string json = "{\"devices\":[";
    bool first = true;
    for (const auto& adapter : gAdapters) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"interface\":" + JsonString(adapter.ifname) +
                ",\"mac\":" + JsonString(adapter.mac) +
                ",\"device\":" + JsonString(adapter.device) +
                ",\"phy\":" + JsonString(adapter.phy) +
                ",\"builtIn\":" + (adapter.built_in ? "true" : "false") +
                ",\"simulated\":" + (adapter.simulated ? "true" : "false") +
                ",\"selected\":" + (adapter.mac == gSelectedKey ? "true" : "false") + "}";
    }
    json += "]}";
    if (json.size() + 1 > capacity) return -1;
    std::memcpy(result, json.c_str(), json.size() + 1);
    return 0;
}

int HandleSelectDevice(const char* args, char* result, size_t capacity, void*) {
    std::string choice;
    if (!ParseDeviceArgument(args, &choice)) return -1;
    if (choice.size() >= PROP_VALUE_MAX) return -1;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        const auto found = std::find_if(gAdapters.begin(), gAdapters.end(), [&](const Adapter& a) {
            return !a.simulated && MatchesChoice(a, choice);
        });
        if (found == gAdapters.end()) return -1;
    }
    if (__system_property_set(kPersistChoice, choice.c_str()) != 0) return -1;
    const std::string json = "{\"selected\":" + JsonString(choice) + "}";
    if (json.size() + 1 > capacity) return -1;
    std::memcpy(result, json.c_str(), json.size() + 1);
    return 0;
}

void PublishSnapshot(const std::vector<Adapter>& adapters, const std::string& selected_key,
                     bool present, bool test_ap_ready) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        changed = gSelectedKey != selected_key || gPresent != present ||
                  gTestApReady != test_ap_ready || gAdapters.size() != adapters.size();
        if (!changed) {
            for (size_t i = 0; i < adapters.size(); ++i) {
                if (gAdapters[i].ifname != adapters[i].ifname ||
                    gAdapters[i].mac != adapters[i].mac) {
                    changed = true;
                    break;
                }
            }
        }
        gAdapters = adapters;
        gSelectedKey = selected_key;
        gPresent = present;
        gTestApReady = test_ap_ready;
    }
    if (changed && gIpcServer) {
        char json[1024];
        if (HandleGetState(nullptr, json, sizeof(json), nullptr) == 0) {
            matonos_ipc_publish(gIpcServer, "state", json);
        } else {
            ALOGW("Wi-Fi state snapshot exceeded IPC buffer");
        }
    }
}

std::string RequestedHwsimRadios() {
    char requested_radios[PROP_VALUE_MAX] = {};
    if (__system_property_get(kPersistHwsimRadiosProp, requested_radios) <= 0) {
        std::strcpy(requested_radios, "1");
    }
    return std::string(requested_radios) == "2" ? "2" : "1";
}

bool EnsureHwsimRadio() {
    if (fs::exists("/sys/module/mac80211_hwsim")) return true;
    Publish(kHwsimRadiosProp, RequestedHwsimRadios());
    ALOGI("requesting the no-hardware mac80211_hwsim fallback radio");
    return Publish(kHwsimLoadProp, "1");
}

}  // namespace

int main() {
    ALOGI("MatonOS Wi-Fi hardware manager started");
    gIpcServer = matonos_ipc_create("wifi");
    if (!gIpcServer ||
        matonos_ipc_register(gIpcServer, "get_state", HandleGetState, nullptr) != 0 ||
        matonos_ipc_register(gIpcServer, "list_devices", HandleListDevices, nullptr) != 0 ||
        matonos_ipc_register(gIpcServer, "select_device", HandleSelectDevice, nullptr) != 0 ||
        matonos_ipc_start(gIpcServer) != 0) {
        ALOGE("cannot register MatonOS Wi-Fi channel service");
        return 1;
    }
    std::string selected_key;
    std::string last_requested_choice = ReadChoice();
    const auto started = std::chrono::steady_clock::now();

    for (;;) {
        std::vector<Adapter> adapters = Enumerate();
        std::vector<Adapter> physical;
        std::copy_if(adapters.begin(), adapters.end(), std::back_inserter(physical),
                     [](const Adapter& a) { return !a.simulated; });
        const std::string persisted = ReadChoice();
        const bool user_changed_choice = persisted != last_requested_choice;
        last_requested_choice = persisted;
        auto selected = adapters.end();
        const bool boot_grace_elapsed = std::chrono::steady_clock::now() - started >= 10s;

        if (!selected_key.empty()) {
            selected = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) {
                return a.mac == selected_key;
            });
            // A real adapter always replaces the synthetic radio. During the
            // first coldplug window, retain hwsim briefly so late built-in
            // adapters can be compared against USB devices before boot choice.
            if (selected != adapters.end() && selected->simulated && !physical.empty() &&
                boot_grace_elapsed) {
                selected = adapters.end();
            }
            if (selected == adapters.end()) selected_key.clear();
        }
        if (selected != adapters.end() && !selected->simulated && user_changed_choice &&
            !persisted.empty() && !MatchesChoice(*selected, persisted)) {
            const auto requested = std::find_if(adapters.begin(), adapters.end(),
                    [&](const Adapter& adapter) {
                        return !adapter.simulated && MatchesChoice(adapter, persisted);
                    });
            if (requested != adapters.end()) selected = requested;
        }
        if (selected == adapters.end() && !physical.empty() && !persisted.empty()) {
            selected = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) {
                if (a.simulated) return false;
                return MatchesChoice(a, persisted);
            });
        }

        if (selected == adapters.end() && boot_grace_elapsed && !physical.empty()) {
            selected = std::find_if(adapters.begin(), adapters.end(),
                                    [](const Adapter& a) { return !a.simulated; });
        }

        if (selected == adapters.end() && !boot_grace_elapsed) {
            // Load the synthetic adapter during coldplug so system_server sees
            // wlan0, but hold it for the complete selection window when a
            // physical device may still arrive.
            EnsureHwsimRadio();
            adapters = Enumerate();
            selected = std::find_if(adapters.begin(), adapters.end(),
                                    [](const Adapter& a) { return a.simulated; });
        }

        if (physical.empty() && boot_grace_elapsed) {
            // Keep an inert, standard nl80211 radio present when a generic PC
            // has no hardware. This lets Android's ordinary Wi-Fi UI remain
            // active and makes a later physical hotplug a normal interface
            // handoff instead of a service restart.
            EnsureHwsimRadio();
            adapters = Enumerate();
            selected = adapters.end();
            if (!selected_key.empty()) {
                selected = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) {
                    return a.mac == selected_key;
                });
            }
            if (selected == adapters.end()) {
                selected = std::find_if(adapters.begin(), adapters.end(),
                                        [](const Adapter& a) { return a.simulated; });
            }
        }

        if (selected == adapters.end()) {
            Publish(kPresentProp, "0");
            Publish(kSelectedProp, "");
            Publish(kHwsimTestReadyProp, "0");
            PublishSnapshot(adapters, "", false, false);
            ApplyRfkill(adapters, "");
            std::this_thread::sleep_for(2s);
            continue;
        }

        const std::string candidate_key = selected->mac;
        const bool reserve_hwsim_test_radio = selected->simulated &&
                                               RequestedHwsimRadios() == "2";
        const std::string test_ap_key = KeepOthersOut(adapters, candidate_key,
                                                       reserve_hwsim_test_radio);
        if (selected->ifname != kWifiIfname && !Rename(selected->ifname, kWifiIfname)) {
            selected_key.clear();
            SetInterfaceDown(selected->ifname);
            Publish(kPresentProp, "0");
            Publish(kSelectedProp, "");
            Publish(kHwsimTestReadyProp, "0");
            PublishSnapshot(adapters, "", false, false);
            std::this_thread::sleep_for(1s);
            continue;
        }

        selected_key = candidate_key;
        ApplyRfkill(adapters, selected_key, test_ap_key);
        Publish(kSelectedProp, selected->simulated ? "hwsim:" + selected_key : selected_key);
        Publish(kPresentProp, "1");
        const bool test_ap_ready = !test_ap_key.empty() && if_nametoindex("wlan1") != 0;
        Publish(kHwsimTestReadyProp, test_ap_ready ? "1" : "0");
        PublishSnapshot(adapters, selected_key, true, test_ap_ready);
        std::this_thread::sleep_for(2s);
    }
}
