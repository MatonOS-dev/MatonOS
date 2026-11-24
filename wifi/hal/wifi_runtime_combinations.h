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
