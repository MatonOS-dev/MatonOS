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

#pragma once

// go/keep-sorted start
#include <audio_utils/clock.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utils/Timers.h>
// go/keep-sorted end

namespace android::audio_utils {

/**
 * Returns the offset to add to a `SYSTEM_TIME_REALTIME` timestamp to get a
 * `SYSTEM_TIME_BOOTTIME` timestamp.
 *
 * @return The offset in nanoseconds.
 */
int64_t getSystemTimeToBootTimeOffset();

/**
 * Returns the offset to add to a `SYSTEM_TIME_BOOTTIME` timestamp to get a
 * `SYSTEM_TIME_REALTIME` timestamp.
 *
 * @return The offset in nanoseconds.
 */
int64_t getBootTimeToSystemTimeOffset();

/**
 * Re-computes and updates the cached offset between `SYSTEM_TIME_REALTIME` and
 * `SYSTEM_TIME_BOOTTIME`. This may be useful to call periodically to account
 * for clock drift.
 */
void updateSystemTimeToBootTimeOffset();

/**
 * Computes the offset between two clocks.
 *
 * This is done by taking a timestamp from each clock as close together as
 * possible. To improve accuracy, this is done three times, and the result
 * with the minimum gap between the two timestamp calls is used.
 *
 * The ids are, for example, 'SYSTEM_TIME_BOOTTIME' or 'SYSTEM_TIME_REALTIME'.
 *
 * @param id1 The first clock id.
 * @param id2 The second clock id.
 * @return The offset in nanoseconds to add to a timestamp from clock id1
 *         to get a timestamp from clock id2.
 */
int64_t computeTimeOffset(int id1, int id2);

/**
 * @brief Adjusts a time offset by re-computing it and applying a tolerance.
 *
 * This templated function re-computes the offset between two clocks using
 * `computeTimeOffset` and updates the provided `offset` pointer only if the
 * measured difference is greater than a specified tolerance.
 * This prevents frequent, minor adjustments due to small drifts.
 * The time offset is expected to change most for the MONOTONIC clock, as it does not
 * increment on suspend.
 *
 * @tparam Offset The type of the offset, typically an (atomic) `int64_t*`.
 * @param id1 The identifier for the first clock (e.g., `SYSTEM_TIME_MONOTONIC`).
 * @param id2 The identifier for the second clock (e.g., `SYSTEM_TIME_BOOTTIME`).
 * @param offset A pointer to the offset variable to be adjusted. This value
 *               will be updated with the `measured` offset if the absolute
 *               difference between the current `*offset` and `measured` is
 *               greater than `kToleranceNs`.
 */
template <typename Offset>
void adjustTimeOffset(int id1, int id2, Offset* offset) {
    const int64_t measured = computeTimeOffset(id1, id2);
    // To avoid micro-adjusting, we don't change the timebase
    // unless it is significantly different.
    //
    // The tolerance should be less than 500us to
    // prevent making a noticeable difference in the
    // logcat printing.
    constexpr int64_t kToleranceNs = 10'000; // 10 us
    if (std::abs(*offset - measured) > kToleranceNs) {
        *offset = measured;
    }
}

/**
 * Formats a system time in nanoseconds into a human-readable string.
 * The format is "HH:MM:SS.MSc", representing hours, minutes, seconds,
 * and milliseconds.
 *
 * @param systemTime The system time in nanoseconds since epoch.
 * @return A string representing the formatted time.
 */
inline std::string formatSystemTime(int64_t systemTime) {
    const auto time_string = audio_utils_time_string_from_ns(systemTime);

    // The time string is 19 characters (including null termination).
    // Example: "03-27 16:47:06.187"
    //           MM DD HH MM SS MS
    // We offset by 6 to get HH:MM:SS.MSc
    //
    return time_string.time + 6; // offset to remove month/day.
}

/**
 * Formats a boot (elapsed) time in nanoseconds into a human-readable string.
 * The format is "HH:MM:SS.MSc", representing hours, minutes, seconds,
 * and milliseconds.
 *
 * @param bootTime The boot (elapsed) time in nanoseconds since epoch.
 * @return A string representing the formatted time.
 */
inline std::string formatBootTime(int64_t bootTime) {
    return formatSystemTime(bootTime + getBootTimeToSystemTimeOffset());
}

/**
 * Formats a std::chrono::system_clock::time_point into a human-readable string.
 * The format is "HH:MM:SS.MSc".
 *
 * @param t The time_point to format.
 * @return A string representing the formatted time.
 */
inline std::string formatTime(std::chrono::system_clock::time_point t) {
    return formatSystemTime(std::chrono::nanoseconds(t.time_since_epoch()).count());
}

/**
 * Finds the position where the common prefix of two time strings ends.
 * This is useful for abbreviated printing of sequential timestamps by
 * removing the duplicated parts.
 *
 * @param time1 The first time string.
 * @param time2 The second time string.
 * @return The index position where the common prefix ends.
 */
inline size_t commonTimePrefixPosition(std::string_view time1, std::string_view time2) {
    const size_t endPos = std::min(time1.size(), time2.size());
    size_t i;

    // Find location of the first mismatch between strings
    for (i = 0; ; ++i) {
        if (i == endPos) {
            return i; // strings match completely to the length of one of the strings.
        }
        if (time1[i] != time2[i]) {
            break;
        }
        if (time1[i] == '\0') {
            return i; // "printed" strings match completely.  No need to check further.
        }
    }

    // Go backwards until we find a delimiter or space.
    for (; i > 0
           && isdigit(time1[i]) // still a number
           && time1[i - 1] != ' '
            ; --i) {
    }
    return i;
}

/**
 * Returns the unique suffix of the second time string that is not present in the first.
 * If the two strings are identical, an empty string_view is returned.
 * This is used to elide the common prefix when printing a series of times.
 *
 * @param time1 The reference time string.
 * @param time2 The time string to find the unique suffix of.
 * @return A string_view containing the unique suffix of time2.
 */
inline std::string_view uniqueTimeSuffix(std::string_view time1, std::string_view time2) {
    const size_t pos = commonTimePrefixPosition(time1, time2);
    return time2.substr(pos);
}

/**
 * A function object that formats a system time in nanoseconds to a string.
 * By default, it uses the formatSystemTime function.
 */
inline std::function<std::string(int64_t)>
systemTimeFormatter = [](int64_t timeNs) { return formatSystemTime(timeNs); };

/**
 * Converts a deque of time pairs (start and end times) into a formatted string.
 * This function allows for custom formatting of the timestamps.
 *
 * @param timePairs A deque of pairs, where each pair represents a start and end time
 *                  in nanoseconds.
 * @param formatter A function that takes a 64-bit integer time and returns a
 *                  formatted string (representing the time).
 * @return A string representing the formatted time pairs.
 */
inline std::string timePairsToString(const std::deque<std::pair<int64_t, int64_t>>& timePairs,
        const std::function<std::string(int64_t)>& formatter) {
    std::string s("{ ");
    std::string lastTime;
    for (const auto& [start, end] : timePairs) {
        std::string startTime = formatter(start);
        std::string endTime = formatter(end);
        const size_t posStart = commonTimePrefixPosition(lastTime, endTime);
        const size_t posEnd =  commonTimePrefixPosition(startTime, endTime);
        s.append("{ ");
        if (posStart) s.append("~");
        s.append(startTime.substr(posStart));
        s.append(", ");
        if (posEnd) s.append("~");
        s.append(endTime.substr(posEnd));
        s.append("} ");
        lastTime = std::move(startTime);  // use startTime to ensure easier readability
    }
    s.append("}");
    return s;
}

/**
 * Converts a deque of system time pairs to a formatted string using the default
 * system time formatter.
 *
 * @param timePairs A deque of pairs, where each pair represents a start and end
 *                  system time in nanoseconds.
 * @return A string representing the formatted system time pairs.
 */
inline std::string systemTimePairsToString(
        const std::deque<std::pair<int64_t, int64_t>>& timePairs) {
    return timePairsToString(timePairs, systemTimeFormatter);
}

/**
 * Converts a deque of boot time pairs to a formatted string.
 * The boot times are converted to system time before formatting.
 *
 * @param timePairs A deque of pairs, where each pair represents a start and end
 *                  boot time in nanoseconds.
 * @return A string representing the formatted time pairs in system time.
 */
std::string bootTimePairsToString(
        const std::deque<std::pair<int64_t, int64_t>>& timePairs);

} // namespace android::audio_utils
