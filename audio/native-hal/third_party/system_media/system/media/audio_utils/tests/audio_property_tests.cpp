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

#include <audio_utils/property.h>
#include <gtest/gtest.h>
#include <map>
#include <string>

// Expose the internal implementation for testing
namespace android::audio_utils::property {

bool isRelaxedTimingDeviceImpl(
        const std::function<bool(const char*, bool)>& get_bool,
        const std::function<std::string(const char*)>& get_prop);

} // namespace android::audio_utils::property

using namespace android::audio_utils::property;

class AudioPropertyTest : public ::testing::Test {
protected:
    std::map<std::string, std::string> props;

    auto getter() {
        return [this](const char* key) {
            return props.count(key) ? props[key] : "";
        };
    }

    auto bool_getter() {
        return [this](const char* key, bool def) {
            if (props.count(key)) {
                return props[key] == "1" || props[key] == "true";
            }
            return def;
        };
    }
};

TEST_F(AudioPropertyTest, PhysicalDevice) {
    props["ro.product.system.brand"] = "google";
    props["ro.product.system.model"] = "Pixel 8";
    props["ro.product.system.name"] = "shiba";
    EXPECT_FALSE(isRelaxedTimingDeviceImpl(bool_getter(), getter()));
}

TEST_F(AudioPropertyTest, Emulator) {
    props["ro.boot.qemu"] = "true";
    EXPECT_TRUE(isRelaxedTimingDeviceImpl(bool_getter(), getter()));
}

TEST_F(AudioPropertyTest, Cuttlefish) {
    props["ro.product.system.brand"] = "google";
    props["ro.product.system.name"] = "cf_x86_64_phone";
    EXPECT_TRUE(isRelaxedTimingDeviceImpl(bool_getter(), getter()));
}

TEST_F(AudioPropertyTest, GSI) {
    props["ro.product.system.brand"] = "generic";
    props["ro.product.system.name"] = "gsi_arm64";
    props["ro.product.system.model"] = "GSI on ARM64";
    EXPECT_TRUE(isRelaxedTimingDeviceImpl(bool_getter(), getter()));
}

TEST_F(AudioPropertyTest, SystemExtOverride) {
    props["ro.product.system.brand"] = "google";
    props["ro.product.system.name"] = "shiba";
    props["ro.product.system_ext.name"] = "aosp_cf_x86_64";
    // Even if system.name is shiba, system_ext.name override should trigger Cuttlefish detection
    EXPECT_TRUE(isRelaxedTimingDeviceImpl(bool_getter(), getter()));
}

TEST(audio_utils_property, Consistency) {
    // Verify the public API is callable and consistent
    const bool result1 = isRelaxedTimingDevice();
    const bool result2 = isRelaxedTimingDevice();
    EXPECT_EQ(result1, result2);
}
