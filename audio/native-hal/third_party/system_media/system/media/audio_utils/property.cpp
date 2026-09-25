/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_utils::threads"

#include <audio_utils/property.h>

#include <atomic>
#include <cutils/properties.h>
#include <functional>
#include <string>
#include <utils/Log.h>

namespace android::audio_utils::property {

/**
 * Internal implementation that allows injecting property getters for testing.
 */
bool isRelaxedTimingDeviceImpl(
        const std::function<bool(const char*, bool)>& get_bool,
        const std::function<std::string(const char*)>& get_prop) {
    // Check if running on the emulator
    if (get_bool("ro.boot.qemu", false)) {
        ALOGD("%s: emulator (qemu) detected", __func__);
        return true;
    }

    // See MediaUtils.java (onFrankenDevice)
    std::string systemBrand = get_prop("ro.product.system.brand");
    std::string systemModel = get_prop("ro.product.system.model");
    std::string systemProduct = get_prop("ro.product.system.name");

    // not all devices may have system_ext partition, but if they do use that
    std::string value = get_prop("ro.product.system_ext.name");
    if (!value.empty()) systemProduct = value;
    value = get_prop("ro.product.system_ext.model");
    if (!value.empty()) systemModel = value;

    if ((systemBrand == "Android" || systemBrand == "generic" || systemBrand == "mainline") &&
        (systemModel.starts_with("AOSP on ") || systemProduct.starts_with("aosp_") ||
         systemModel.starts_with("GSI on ") || systemProduct.starts_with("gsi_"))) {
        ALOGD("%s: Android/Generic/Mainline on AOSP or GSI detected", __func__);
        return true;
    } else if ((systemBrand == "Android" || systemBrand == "google") &&
        (systemProduct.starts_with("cf_") || systemProduct.starts_with("aosp_cf_") ||
         systemModel.starts_with("Cuttlefish "))) {
        ALOGD("%s: Cuttlefish detected", __func__);
        return true;
    }
    return false;
}

bool isRelaxedTimingDevice() {
    // lock-free cache check.
    static constinit std::atomic<int> relaxedState{}; // 0: uninitialized, -1: false, 1: true
    if (int state = relaxedState.load(std::memory_order_relaxed)) {
        return state == 1;
    }

    // it is harmless for multiple threads to evaluate the following
    // code in parallel, should there be a race on initialization.

    bool isRelaxed = isRelaxedTimingDeviceImpl(
        [](const char* key, bool def) { return property_get_bool(key, def); },
        [](const char* key) {
            char value[PROPERTY_VALUE_MAX];
            return property_get(key, value, nullptr) > 0 ? std::string(value) : "";
        }
    );

    relaxedState.store(isRelaxed ? 1 : -1, std::memory_order_relaxed);
    return isRelaxed;
}

}  // namespace android::audio_utils::property
