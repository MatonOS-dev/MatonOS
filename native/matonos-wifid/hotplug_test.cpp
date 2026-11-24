// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#include "hotplug_payload.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {
void Check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << '\n';
        std::exit(1);
    }
    std::cout << "PASS " << name << '\n';
}
}

int main() {
    using MacSet = std::set<std::string>;
    const std::string a = "00:11:22:33:44:55";
    const std::string b = "00:11:22:33:44:66";
    auto result = wifid::BuildDeviceChange({a}, {a, b}, a, a);
    Check(result.changed && result.payload ==
          "{\"added\":[\"00:11:22:33:44:66\"],\"removed\":[],\"preferred_present\":true,\"wlan0\":\"00:11:22:33:44:55\"}", "added");
    result = wifid::BuildDeviceChange({a, b}, {b}, a, "");
    Check(result.changed && result.payload ==
          "{\"added\":[],\"removed\":[\"00:11:22:33:44:55\"],\"preferred_present\":false,\"wlan0\":\"\"}", "removed/preferred absent/empty wlan0");
    result = wifid::BuildDeviceChange({a}, {b}, b, b);
    Check(result.changed && result.payload.find("\"added\":[\"00:11:22:33:44:66\"]") != std::string::npos &&
          result.payload.find("\"removed\":[\"00:11:22:33:44:55\"]") != std::string::npos, "both");
    result = wifid::BuildDeviceChange({a}, {a}, a, a);
    Check(!result.changed && result.payload.find("\"added\":[]") != std::string::npos &&
          result.payload.find("\"removed\":[]") != std::string::npos, "unchanged");
    // Interface names are intentionally absent from the diff: a SwitchTo rename
    // that keeps the same permanent MAC set is unchanged.
    const MacSet before_rename{a};
    const MacSet after_rename{a};
    result = wifid::BuildDeviceChange(before_rename, after_rename, a, a);
    Check(!result.changed, "rename only");
    return 0;
}
