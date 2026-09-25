/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <audio_utils/Time.h>

#include <gtest/gtest.h>
#include <utils/Timers.h>

using namespace android::audio_utils;

TEST(time_utilities, formatSystemTime) {
    // Basic checks that hold regardless of timezone.
    // Start of epoch is at 0 seconds.
    EXPECT_TRUE(formatSystemTime(0).ends_with(":00.000"));

    // 1 second after epoch is 1 second.
    EXPECT_TRUE(formatSystemTime(1'000'000'000).ends_with(":01.000"));

    // A specific time point.
    // Corresponds to 2023-10-27 10:00:00 UTC
    const int64_t time_ns = 1698397200000000000;
    EXPECT_TRUE(formatSystemTime(time_ns).ends_with("00:00.000"));
}

TEST(time_utilities, commonTimePrefixPosition) {
    EXPECT_EQ(0U, commonTimePrefixPosition("", ""));
    EXPECT_EQ(0U, commonTimePrefixPosition("abc", ""));
    EXPECT_EQ(0U, commonTimePrefixPosition("", "abc"));
    EXPECT_EQ(3U, commonTimePrefixPosition("abc", "abc"));
    EXPECT_EQ(2U, commonTimePrefixPosition("abc", "abd"));
    EXPECT_EQ(0U, commonTimePrefixPosition("abc", "def"));

    // Test with time-like strings
    EXPECT_EQ(12U, commonTimePrefixPosition("12:34:56.789", "12:34:56.789"));
    EXPECT_EQ(8U, commonTimePrefixPosition("12:34:56.789", "12:34:56.780"));  // goes to the .
    EXPECT_EQ(5U, commonTimePrefixPosition("12:34:56.789", "12:34:57.789"));  // goes to the :
    EXPECT_EQ(0U, commonTimePrefixPosition("12:34:56.789", "22:34:56.789"));
}

TEST(time_utilities, uniqueTimeSuffix) {
    EXPECT_EQ("", std::string(uniqueTimeSuffix("", "")));
    EXPECT_EQ("abc", std::string(uniqueTimeSuffix("", "abc")));
    EXPECT_EQ("", std::string(uniqueTimeSuffix("abc", "")));
    EXPECT_EQ("", std::string(uniqueTimeSuffix("abc", "abc")));
    EXPECT_EQ("d", std::string(uniqueTimeSuffix("abc", "abd")));
    EXPECT_EQ("def", std::string(uniqueTimeSuffix("abc", "def")));

    // Test with time-like strings
    const std::string time0 = "10:00:00.000";
    const std::string time1 = "10:00:01.000";
    EXPECT_EQ(":01.000", std::string(uniqueTimeSuffix(time0, time1)));
    EXPECT_EQ("", std::string(uniqueTimeSuffix(time0, time0)));
    EXPECT_EQ("10:00:01.000", std::string(uniqueTimeSuffix("", time1)));
}

TEST(time_utilities, timePairsToString) {
    const std::deque<std::pair<int64_t, int64_t>> empty_pairs;
    EXPECT_EQ("{ }", timePairsToString(empty_pairs, systemTimeFormatter));

    const std::deque<std::pair<int64_t, int64_t>> pairs = {
        {1698397200000000000, 1698397201000000000}, // 10:00:00.000 to 10:00:01.000
        {1698397202000000000, 1698397203000000000}  // 10:00:02.000 to 10:00:03.000
    };

    const std::string expected_suffix =
        ":00:00.000, ~:01.000} { ~:02.000, ~:03.000} }";  // omitting "{{ 10"
    const std::string actual = systemTimePairsToString(pairs);
    EXPECT_TRUE(actual.ends_with(expected_suffix));
}

TEST(time_utilities, BootTime) {
    EXPECT_EQ(getSystemTimeToBootTimeOffset(), -getBootTimeToSystemTimeOffset());

    const int64_t offset = systemTime(SYSTEM_TIME_REALTIME) - systemTime(SYSTEM_TIME_BOOTTIME);
    EXPECT_NEAR(offset, getBootTimeToSystemTimeOffset(), 3'000'000'000LL); // 3 seconds
}

TEST(time_utilities, adjustTimeOffset) {
    // We can't directly mock computeTimeOffset, so we'll test its interaction
    // with adjustTimeOffset by manipulating the initial offset value.

    // Realtime and Boottime differences should be relatively fixed
    // even during suspend.

    // 100ms near tolerance for test stability
    constexpr int64_t kOffsetToleranceNs = 100'000'000;

    // Scenario 1: Initial offset is far from the actual measured offset, should update.
    int64_t currentOffset = 0; // Arbitrary initial value
    // The actual measured offset will be around
    // systemTime(SYSTEM_TIME_REALTIME) - systemTime(SYSTEM_TIME_BOOTTIME)
    // which is typically a large number (e.g., billions of nanoseconds) for the realtime clock.
    // So, 0 is far enough to trigger an update.
    adjustTimeOffset(SYSTEM_TIME_REALTIME, SYSTEM_TIME_BOOTTIME, &currentOffset);
    // Expect currentOffset to be updated to a value close to the actual system time offset.
    EXPECT_LT(currentOffset, 0);  // need to subtract from realtime to get boottime.
    EXPECT_NEAR(currentOffset, getSystemTimeToBootTimeOffset(), kOffsetToleranceNs);

    // Scenario 2: Initial offset is close to the actual measured offset, should NOT update.
    // Set currentOffset to be very close to the actual offset, within kToleranceNs (10us).
    currentOffset = getSystemTimeToBootTimeOffset();
    // Add a small delta, less than kToleranceNs (10us)
    currentOffset += 5'000; // 5us

    int64_t originalOffset = currentOffset;
    adjustTimeOffset(SYSTEM_TIME_REALTIME, SYSTEM_TIME_BOOTTIME, &currentOffset);
    // Expect currentOffset to remain unchanged because the difference is within tolerance.
    EXPECT_EQ(originalOffset, currentOffset);

    // Scenario 3: Initial offset is far from the actual measured offset, should update.
    currentOffset = getSystemTimeToBootTimeOffset();
    // Add a large delta, more than kToleranceNs (10us)
    currentOffset += 20'000; // 20us

    adjustTimeOffset(SYSTEM_TIME_REALTIME, SYSTEM_TIME_BOOTTIME, &currentOffset);
    // Expect currentOffset to be updated to a value close to the actual system time offset.
    EXPECT_NEAR(currentOffset, getSystemTimeToBootTimeOffset(), kOffsetToleranceNs);
}
