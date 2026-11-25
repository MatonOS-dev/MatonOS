// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define WIFID_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "matonos-wifid", __VA_ARGS__)
#else
#define WIFID_LOGE(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

#define WIFID_CLOSE(fd) do { \
    const int close_fd = (fd); \
    const int saved_errno = errno; \
    if (::close(close_fd) != 0) { \
        const int close_error = errno; \
        WIFID_LOGE("close fd=%d failed errno=%d (%s) at %s:%d", \
                     close_fd, close_error, std::strerror(close_error), __FILE__, __LINE__); \
    } \
    errno = saved_errno; \
} while (0)

namespace wifid {
struct SwitchInterface {
    std::string name;
    bool simulated;
};

inline bool NameExists(const std::string& name) {
    struct if_nameindex* names = if_nameindex();
    if (!names) {
        const int error = errno;
        WIFID_LOGE("if_nameindex failed errno=%d (%s)", error, std::strerror(error));
        errno = error;
        return false;
    }
    bool found = false;
    for (auto* item = names; item->if_index && !found; ++item) found = name == item->if_name;
    if_freenameindex(names);
    return found;
}

inline int Rename(int fd, const std::string& from, const std::string& to) {
    if (to.size() >= IFNAMSIZ) { errno = ENAMETOOLONG; return -1; }
    struct ifreq request{};
    std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", from.c_str());
    std::snprintf(request.ifr_newname, sizeof(request.ifr_newname), "%s", to.c_str());
    return ioctl(fd, SIOCSIFNAME, &request);
}

inline int SetUp(int fd, const std::string& name, bool up) {
    struct ifreq request{};
    std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name.c_str());
    if (ioctl(fd, SIOCGIFFLAGS, &request) != 0) return -1;
    if (up) request.ifr_flags |= IFF_UP;
    else request.ifr_flags &= static_cast<short>(~IFF_UP);
    return ioctl(fd, SIOCSIFFLAGS, &request);
}

inline int SwitchTo(const std::vector<SwitchInterface>& interfaces, const std::string& chosen,
                    std::string* failure) {
    for (const auto& item : interfaces) if (item.name == chosen && item.name == "wlan0") return 0;
    bool found = false;
    for (const auto& item : interfaces) if (item.name == chosen && !item.simulated) found = true;
    if (!found) { errno = ENODEV; if (failure) *failure = "chosen interface is absent or simulated"; return -1; }

    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { if (failure) *failure = "socket: " + std::string(strerror(errno)); return -1; }
    struct Saved { std::string original, current; bool up; bool simulated; };
    std::vector<Saved> saved;
    for (const auto& item : interfaces) {
        struct ifreq request{};
        std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", item.name.c_str());
        if (ioctl(fd, SIOCGIFFLAGS, &request) != 0) {
            if (failure) *failure = "SIOCGIFFLAGS " + item.name + ": " + strerror(errno);
            WIFID_CLOSE(fd); return -1;
        }
        saved.push_back({item.name, item.name, (request.ifr_flags & IFF_UP) != 0, item.simulated});
    }
    auto set_failure = [&](const std::string& operation, const std::string& name) {
        if (failure) *failure = operation + " " + name + ": " + strerror(errno);
    };
    bool ok = true;
    for (auto& item : saved) {
        if (SetUp(fd, item.current, false) != 0) { set_failure("bring down", item.current); ok = false; break; }
    }
    auto unique_name = [&](const char* prefix, unsigned* counter) {
        for (unsigned attempt = 0; attempt < 4096; ++attempt) {
            std::string name = std::string(prefix) + std::to_string((*counter)++);
            if (name.size() < IFNAMSIZ && !NameExists(name)) return name;
        }
        errno = ENOSPC;
        return std::string();
    };
    unsigned temp_id = 0;
    if (ok) for (auto& item : saved) {
        const std::string next = unique_name("wtmp", &temp_id);
        if (next.empty() || Rename(fd, item.current, next) != 0) { set_failure("rename " + item.current + " to", next); ok = false; break; }
        item.current = next;
    }
    unsigned other_id = 0;
    if (ok) for (auto& item : saved) {
        std::string next;
        if (item.original == chosen) next = "wlan0";
        else if (item.simulated && item.original != "wlan0") next = item.original;
        else {
            bool allocated = false;
            errno = ENOSPC;
            for (unsigned attempt = 0; attempt < 4096; ++attempt) {
                next = "wlan_int" + std::to_string(other_id++);
                if (next.size() >= IFNAMSIZ) { errno = ENAMETOOLONG; break; }
                if (!NameExists(next)) { allocated = true; break; }
            }
            if (!allocated) {
                if (errno != ENAMETOOLONG) errno = ENOSPC;
                set_failure("allocate name for", item.original);
                ok = false;
                break;
            }
            if (!ok) break;
        }
        if (Rename(fd, item.current, next) != 0) { set_failure("rename " + item.current + " to", next); ok = false; break; }
        item.current = next;
    }
    if (ok) for (auto& item : saved) {
        if (item.up && SetUp(fd, item.current, true) != 0) { set_failure("restore up state", item.current); ok = false; break; }
    }
    if (!ok) {
        // Two passes: park every renamed interface on a free temporary name
        // first, so an original name still held by another card is released
        // before anything is renamed back onto it.
        for (auto& item : saved) {
            if (item.current == item.original) continue;
            const std::string park = unique_name("wtmp", &temp_id);
            if (park.empty() || Rename(fd, item.current, park) != 0) {
                if (failure) *failure += "; rollback park " + item.current + ": " + strerror(errno);
                continue;
            }
            item.current = park;
        }
        for (auto& item : saved) {
            if (item.current != item.original && Rename(fd, item.current, item.original) != 0 && failure)
                *failure += "; rollback rename " + item.current + " to " + item.original + ": " + strerror(errno);
            if (SetUp(fd, item.original, item.up) != 0 && failure)
                *failure += "; rollback flags " + item.original + ": " + strerror(errno);
        }
    }
    WIFID_CLOSE(fd);
    return ok ? 0 : -1;
}
}  // namespace wifid
