/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "matonos-btd"

#include <sys/system_properties.h>
#include "maton_log.h"
#include "vhci_emulator.h"
#include "matonos_ipc.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

namespace {
constexpr size_t PROPERTY_VALUE_MAX = 92;
constexpr char kPresent[] = "vendor.maton.bluetooth.present";
constexpr char kAvailable[] = "vendor.maton.bluetooth.available";
constexpr char kVirtual[] = "vendor.maton.bluetooth.virtual";
constexpr char kIndex[] = "vendor.maton.bluetooth.hci_index";
constexpr char kRfkillIndex[] = "vendor.maton.bluetooth.rfkill_index";
constexpr char kSelected[] = "vendor.maton.bluetooth.selected";
constexpr char kChoice[] = "persist.vendor.maton.bluetooth.device";
constexpr char kVhciIndex[] = "vendor.maton.bluetooth.vhci_index";
pid_t g_vhci_pid = -1;

struct Adapter {
    int index;
    std::string hci;
    std::string id;
    std::string sysfs_path;
    bool usb;
};

std::string ReadLink(const std::string& path) {
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved) : std::string();
}

std::string JsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 0x0f]);
        }
        else out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

std::vector<Adapter> Enumerate() {
    std::vector<Adapter> result;
    DIR* dir = opendir("/sys/class/bluetooth");
    if (!dir) return result;
    while (dirent* e = readdir(dir)) {
        if (strncmp(e->d_name, "hci", 3) != 0 || !e->d_name[3]) continue;
        char* end = nullptr;
        long index = strtol(e->d_name + 3, &end, 10);
        if (!end || *end || index < 0 || index > 255) continue;
        std::string hci(e->d_name);
        std::string path = ReadLink("/sys/class/bluetooth/" + hci + "/device");
        if (path.empty()) continue;
        if (path.find("/virtual/") != std::string::npos || path.find("vhci") != std::string::npos)
            continue;
        std::string id = path.substr(path.find_last_of('/') + 1);
        const auto iface = id.find(':');
        if (iface != std::string::npos) id.resize(iface);
        if (id.empty()) id = hci;
        // Device paths below a USB bus are removable dongles. PCIe, SDIO and
        // serdev/UART devices are treated as built-in for boot preference.
        bool usb = path.find("/usb") != std::string::npos || path.find("/1-") != std::string::npos;
        result.push_back({static_cast<int>(index), hci, id, path, usb});
    }
    closedir(dir);
    std::sort(result.begin(), result.end(), [](const Adapter& a, const Adapter& b) {
        return a.index < b.index;
    });
    return result;
}

void SetIfChanged(const char* key, const std::string& value) {
    char old[PROPERTY_VALUE_MAX] = {};
    __system_property_get(key, old);
    if (value != old && __system_property_set(key, value.c_str()) != 0)
        ALOGE("could not set property %s", key);
}

bool GetChoice(std::string* choice) {
    char value[PROPERTY_VALUE_MAX] = {};
    __system_property_get(kChoice, value);
    *choice = value;
    return !choice->empty();
}

const Adapter* FindChoice(const std::vector<Adapter>& adapters, const std::string& choice) {
    for (const auto& a : adapters) {
        if (a.id == choice || a.hci == choice || ("hci" + std::to_string(a.index)) == choice)
            return &a;
    }
    return nullptr;
}

const Adapter* Choose(const std::vector<Adapter>& adapters, const std::string& old_id,
                      const std::string& requested) {
    // A persisted, available device is an explicit selection. This also lets
    // an adapter selected while its preference was absent switch when it is
    // hotplugged later.
    if (const Adapter* preferred = FindChoice(adapters, requested)) return preferred;
    if (const Adapter* current = FindChoice(adapters, old_id)) return current;
    auto built_in = std::find_if(adapters.begin(), adapters.end(), [](const Adapter& a) {
        return !a.usb;
    });
    return built_in == adapters.end() ? (adapters.empty() ? nullptr : &adapters.front()) : &*built_in;
}

int FindRfkillIndex(const std::string& hci) {
    DIR* dir = opendir("/sys/class/rfkill");
    if (!dir) return -1;
    int found = -1;
    while (dirent* e = readdir(dir)) {
        if (strncmp(e->d_name, "rfkill", 6) != 0) continue;
        std::string base = std::string("/sys/class/rfkill/") + e->d_name;
        char name[128] = {};
        FILE* f = fopen((base + "/name").c_str(), "re");
        if (f) { (void)fgets(name, sizeof(name), f); fclose(f); }
        name[strcspn(name, "\r\n")] = 0;
        std::string device = ReadLink(base + "/device");
        bool matches = name == hci || device.find("/" + hci) != std::string::npos;
        if (!matches) continue;
        char* end = nullptr;
        long index = strtol(e->d_name + 6, &end, 10);
        if (!end || *end || index < 0 || index > INT_MAX) continue;
        found = static_cast<int>(index);
        break;
    }
    closedir(dir);
    return found;
}

void SetRfkillIndex(int index, bool block) {
    if (index < 0) return;
    int control = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (control < 0) { ALOGW("rfkill control unavailable: %s", strerror(errno)); return; }
    struct RfkillEvent { uint32_t index; uint8_t type, op, soft, hard; } __attribute__((packed));
    RfkillEvent event{static_cast<uint32_t>(index), 2, 2,
                      static_cast<uint8_t>(block ? 1 : 0), 0};
    if (write(control, &event, sizeof(event)) != static_cast<ssize_t>(sizeof(event)))
        ALOGW("rfkill change for index %d failed: %s", index, strerror(errno));
    close(control);
}

void SetRfkill(const std::string& hci, bool block) {
    SetRfkillIndex(FindRfkillIndex(hci), block);
}

void CheckFirmwareDirectories() {
    static bool checked = false;
    if (checked) return;
    checked = true;
    const char* dirs[] = {"/vendor/firmware/intel", "/vendor/firmware/rtl_bt",
                          "/vendor/firmware/mrvl"};
    for (const char* path : dirs) {
        DIR* dir = opendir(path);
        if (!dir) {
            ALOGW("firmware directory unavailable: %s", path);
            continue;
        }
        bool any = false;
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] != '.') { any = true; break; }
        }
        closedir(dir);
        if (!any) ALOGW("no firmware files found in %s", path);
    }
}

int ListDevices(const char*, char* result, size_t capacity, void*) {
    std::string json = "{\"devices\":[";
    bool first = true;
    for (const auto& a : Enumerate()) {
        if (!first) json += ",";
        first = false;
        json += "{\"id\":" + JsonString(a.id) + ",\"hci\":" + JsonString(a.hci) +
                "\",\"index\":" + std::to_string(a.index) +
                ",\"usb\":" + (a.usb ? "true" : "false") + "}";
    }
    json += "]}";
    if (json.size() + 1 > capacity) return -1;
    memcpy(result, json.c_str(), json.size() + 1);
    return 0;
}

int SelectDevice(const char* args, char* result, size_t capacity, void*) {
    if (!args) return -1;
    const char* key = strstr(args, "\"id\"");
    if (!key || !(key = strchr(key, ':'))) return -1;
    ++key;
    while (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n') ++key;
    if (*key++ != '\"') return -1;
    std::string id;
    while (*key && *key != '\"' && id.size() < 128) {
        const char c = *key++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' )) return -1;
        id.push_back(c);
    }
    if (*key != '\"' || id.empty()) return -1;
    const auto adapters = Enumerate();
    if (!FindChoice(adapters, id)) return -1;
    if (__system_property_set(kChoice, id.c_str()) != 0) return -1;
    const std::string json = "{\"selected\":" + JsonString(id) + "}";
    if (json.size() + 1 > capacity) return -1;
    memcpy(result, json.c_str(), json.size() + 1);
    return 0;
}

int PropertyInt(const char* key, int fallback) {
    char value[PROPERTY_VALUE_MAX] = {};
    if (__system_property_get(key, value) <= 0) return fallback;
    return atoi(value);
}

void StartVhci() {
    if (g_vhci_pid > 0) {
        const pid_t result = waitpid(g_vhci_pid, nullptr, WNOHANG);
        if (result == 0) return;
        if (result < 0 && errno != ECHILD) { ALOGW("waitpid VHCI: %s", strerror(errno)); return; }
        g_vhci_pid = -1;
    }
    SetIfChanged(kVhciIndex, "-1");
    g_vhci_pid = fork();
    if (g_vhci_pid == 0) {
        char executable[PATH_MAX];
        const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length > 0) { executable[length] = '\0'; execl(executable, "matonos-btd", "--vhci", nullptr); }
        _exit(127);
    }
    if (g_vhci_pid < 0) ALOGE("failed to start VHCI emulator: %s", strerror(errno));
}

void StopVhci() {
    if (g_vhci_pid <= 0) return;
    kill(g_vhci_pid, SIGTERM);
    (void)waitpid(g_vhci_pid, nullptr, 0);
    g_vhci_pid = -1;
    SetIfChanged(kVhciIndex, "-1");
}

void Manage() {
    auto adapters = Enumerate();
    CheckFirmwareDirectories();
    std::string old_id;
    char old_index_text[PROPERTY_VALUE_MAX] = {};
    char old_selected[PROPERTY_VALUE_MAX] = {};
    __system_property_get(kIndex, old_index_text);
    __system_property_get(kSelected, old_selected);
    old_id = old_selected;
    std::string requested;
    GetChoice(&requested);
    const Adapter* selected = Choose(adapters, old_id, requested);

    // The HAL unblocks the selected physical controller during initialization.
    for (const auto& a : adapters) if (!selected || a.index != selected->index) SetRfkill(a.hci, true);

    bool changed = false;
    // Keep Android's Bluetooth stack alive with a VHCI controller when no
    // physical radio exists. The emulator never reports advertisements.
    if (!selected) {
        StartVhci();
        const int virtual_index = PropertyInt(kVhciIndex, -1);
        if (old_id.size() && (old_id != "vhci" || atoi(old_index_text) != virtual_index)) {
            SetIfChanged(kAvailable, "0");
            usleep(500000);
        }
        SetIfChanged(kPresent, "0");
        if (virtual_index >= 0) SetIfChanged(kIndex, std::to_string(virtual_index));
        SetIfChanged(kSelected, "vhci");
        SetIfChanged(kRfkillIndex, "-1");
        SetIfChanged(kVirtual, "1");
        if (virtual_index >= 0) SetIfChanged(kAvailable, "1");
    } else {
        char index[16];
        snprintf(index, sizeof(index), "%d", selected->index);
        changed = old_id.size() &&
                  (old_id != selected->id || atoi(old_index_text) != selected->index);
        // Stop the HAL before changing its selected index, then the 0->1
        // property transition starts it again against the new controller.
        if (changed) {
            SetIfChanged(kAvailable, "0");
            usleep(500000);
        }
        StopVhci();
        SetIfChanged(kIndex, index);
        SetIfChanged(kSelected, selected->id);
        SetIfChanged(kRfkillIndex, std::to_string(FindRfkillIndex(selected->hci)));
        SetIfChanged(kPresent, "1");
        SetIfChanged(kVirtual, "0");
        // Availability is written last so the HAL observes a coherent backend.
        SetIfChanged(kAvailable, "1");
    }
    if (changed)
        ALOGI("selected controller changed to %s (%s)", selected->hci.c_str(), selected->id.c_str());
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--vhci") == 0) return RunVhciEmulator();
    MatonosIpcServer* ipc = nullptr;
    ipc = matonos_ipc_create("bluetooth");
    if (!ipc || matonos_ipc_register(ipc, "list_devices", ListDevices, nullptr) != 0 ||
        matonos_ipc_register(ipc, "select_device", SelectDevice, nullptr) != 0 ||
        matonos_ipc_start(ipc) != 0) {
        ALOGE("Bluetooth manager channel registration failed");
        return 1;
    }
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) { ALOGE("uevent socket failed: %s", strerror(errno)); return 1; }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_pid = static_cast<uint32_t>(getpid());
    address.nl_groups = 1;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ALOGE("uevent bind failed: %s", strerror(errno)); close(fd); return 1;
    }
    // USB/serdev discovery can settle after the early-init coldplug event.
    for (int i = 0; i < 60; ++i) {
        Manage();
        pollfd pfd{fd, POLLIN, 0};
        (void)poll(&pfd, 1, 500);
        char event[4096];
        for (;;) {
            if (recv(fd, event, sizeof(event), MSG_DONTWAIT) >= 0) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                ALOGW("uevent recv: %s", strerror(errno));
            break;
        }
    }
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        int result;
        do { result = poll(&pfd, 1, 1000); } while (result < 0 && errno == EINTR);
        if (result > 0) {
            char event[4096];
            for (;;) {
                if (recv(fd, event, sizeof(event), MSG_DONTWAIT) >= 0) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    ALOGW("uevent recv: %s", strerror(errno));
                break;
            }
        }
        // Polling also observes a v2 persisted device choice without requiring
        // a separate binder API; that API can write the same stable device ID.
        Manage();
    }
}
