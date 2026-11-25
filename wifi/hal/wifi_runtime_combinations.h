/*
 * Copyright (C) 2026 MatonOS
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/wifi/IWifiChip.h>

#include <string>
#include <vector>

bool getRuntimeIfaceCombinations(
        const std::string& iface_name,
        std::vector<aidl::android::hardware::wifi::IWifiChip::ChipConcurrencyCombination>* out);

// Generic nl80211 interface management for legacy backends that cannot create
// virtual interfaces themselves (the goldfish backend expects them pre-made).
// |nl80211_type| is an NL80211_IFTYPE_* value; the new interface is created on
// |parent|'s phy.
bool nl80211CreateInterface(const std::string& parent, const std::string& name,
                            uint32_t nl80211_type);
bool nl80211DeleteInterface(const std::string& name);
