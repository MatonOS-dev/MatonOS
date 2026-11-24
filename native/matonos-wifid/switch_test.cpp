// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#include "switch_core.h"

#include <cstdlib>
#include <iostream>

static void Run(const char* command) {
    if (std::system(command) != 0) { std::cerr << "command failed: " << command << '\n'; std::exit(1); }
}

static bool Exists(const char* name) { return if_nametoindex(name) != 0; }

static int Scenario(const std::string& mode) {
    std::string failure;
    if (mode == "already") {
        Run("ip link add wlan0 type dummy");
        if (wifid::SwitchTo({{"wlan0", false}}, "wlan0", &failure) != 0) return 1;
        return 0;
    }
    if (mode == "swap") {
        Run("ip link add wlan0 type dummy && ip link add wlan1 type dummy");
        if (wifid::SwitchTo({{"wlan0", false}, {"wlan1", false}}, "wlan1", &failure) != 0) return 1;
        if (!Exists("wlan0") || !Exists("wlan_int0") || Exists("wlan1")) return 2;
        return 0;
    }
    if (mode == "three") {
        Run("ip link add wlan0 type dummy && ip link add wlan1 type dummy && ip link add wlan2 type dummy");
        if (wifid::SwitchTo({{"wlan0", false}, {"wlan1", false}, {"wlan2", false}}, "wlan2", &failure) != 0) return 1;
        if (!Exists("wlan0") || !Exists("wlan_int0") || !Exists("wlan_int1")) return 2;
        return 0;
    }
    if (mode == "states") {
        Run("ip link add wlan0 type dummy && ip link add wlan1 type dummy && ip link set wlan0 up");
        if (wifid::SwitchTo({{"wlan0", false}, {"wlan1", false}}, "wlan1", &failure) != 0) return 1;
        struct ifreq req{}; int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        std::snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan_int0");
        const bool first_up = ioctl(fd, SIOCGIFFLAGS, &req) == 0 && (req.ifr_flags & IFF_UP);
        std::snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan0");
        const bool second_up = ioctl(fd, SIOCGIFFLAGS, &req) == 0 && (req.ifr_flags & IFF_UP);
        close(fd);
        if (!first_up || second_up) return 2;
        return 0;
    }
    if (mode == "rollback") {
        Run("ip link add wlan0 type dummy && ip link add wlan1 type dummy");
        // wlan_int0 is deliberately not in the Wi-Fi list, so allocation skips it.
        Run("ip link add wlan_int0 type dummy");
        // Force the final wlan0 target to fail via a non-Wi-Fi interface.
        Run("ip link set wlan0 name wlan_aux");
        Run("ip link set wlan_int0 name wlan0");
        const int result = wifid::SwitchTo({{"wlan_aux", false}, {"wlan1", false}}, "wlan1", &failure);
        if (result == 0 || !Exists("wlan_aux") || !Exists("wlan1") || !Exists("wlan0")) return 2;
        if (failure.empty()) return 3;
        std::cout << "rollback failure captured: " << failure << '\n';
        return 0;
    }
    return 64;
}

int main(int argc, char** argv) {
    if (argc != 2) return 64;
    const int result = Scenario(argv[1]);
    if (result != 0) std::cerr << "scenario " << argv[1] << " failed (" << result << "): " << strerror(errno) << '\n';
    return result;
}
