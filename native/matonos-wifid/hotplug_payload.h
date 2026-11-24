// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <set>
#include <string>

namespace wifid {
struct DeviceChange {
    bool changed;
    std::string payload;
};

inline std::string JsonQuote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c >= 0x20) out.push_back(c);
    }
    return out + "\"";
}

inline DeviceChange BuildDeviceChange(const std::set<std::string>& previous,
                                      const std::set<std::string>& current,
                                      const std::string& preferred,
                                      const std::string& wlan0) {
    std::string added = "[", removed = "[";
    bool first_added = true, first_removed = true;
    for (const auto& mac : current) {
        if (previous.count(mac)) continue;
        if (!first_added) added += ',';
        first_added = false;
        added += JsonQuote(mac);
    }
    for (const auto& mac : previous) {
        if (current.count(mac)) continue;
        if (!first_removed) removed += ',';
        first_removed = false;
        removed += JsonQuote(mac);
    }
    added += ']';
    removed += ']';
    const bool changed = previous != current;
    return {changed, "{\"added\":" + added + ",\"removed\":" + removed +
            ",\"preferred_present\":" + (current.count(preferred) ? "true" : "false") +
            ",\"wlan0\":" + JsonQuote(wlan0) + "}"};
}
}  // namespace wifid
