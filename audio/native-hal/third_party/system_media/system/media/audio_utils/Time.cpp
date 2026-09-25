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

//#define LOG_NDEBUG 0
#define LOG_TAG "Time"

#include <audio_utils/Time.h>

// go/keep-sorted start
#include <atomic>
#include <utils/Log.h>
// go/keep-sorted end

namespace android::audio_utils {

// Helper class
class TimeBaseSingleton {
public:
    // The system clock and boot time offsets do not change significantly.
    // Cache this once at the start at this time.
    TimeBaseSingleton() {
        updateSystemTimeToBootTimeOffset();
    }

    static TimeBaseSingleton& get() {
        [[clang::no_destroy]] static TimeBaseSingleton timeBaseSingleton;
        return timeBaseSingleton;
    }

    int64_t getSystemTimeToBootTimeOffset() const {
        return mSystemTimeToBootTime;
    }

    int64_t getBootTimeToSystemTimeOffset() const {
        return -mSystemTimeToBootTime;
    }

    int64_t systemTimeToBootTime(int64_t time) const {
        return time + mSystemTimeToBootTime;
    }

    int64_t bootTimeToSystemTime(int64_t time) const {
        return time - mSystemTimeToBootTime;
    }

    void updateSystemTimeToBootTimeOffset() {
        adjustTimeOffset(SYSTEM_TIME_REALTIME, SYSTEM_TIME_BOOTTIME, &mSystemTimeToBootTime);
    }

protected:
    mutable std::atomic_int64_t mSystemTimeToBootTime = 0;  // set only in ctor
    mutable std::atomic_int64_t mMonotonicTimeToBootTime = 0;
};

int64_t computeTimeOffset(int id1, int id2) {
    // Try three times to get the clock offset, choose the one
    // with the minimum gap in measurements.  This avoids issues
    // with context switches during offset computation.
    constexpr size_t kTries = 3;
    nsecs_t bestGap;
    nsecs_t bestMeasured;
    nsecs_t t1 = systemTime(id1);
    for (size_t i = 0; i < kTries; ++i) {
        // sandwich calls <t1> <t2> <t1_after>
        const nsecs_t t2 = systemTime(id2);
        const nsecs_t t1_after = systemTime(id1);

        const nsecs_t gap = t1_after - t1;
        if (i == 0 || gap < bestGap) {
            bestGap = gap;
            bestMeasured = t2 - ((t1_after + t1) >> 1);
        }
        t1 = t1_after;  // reuse t1 clock sampling.
    }
    return bestMeasured;
}

std::string bootTimePairsToString(
        const std::deque<std::pair<int64_t, int64_t>>& timePairs) {
    const auto bootTimeToSystemTime = TimeBaseSingleton::get().getBootTimeToSystemTimeOffset();
    return timePairsToString(timePairs, [bootTimeToSystemTime](int64_t time) {
        return formatSystemTime(time + bootTimeToSystemTime);});
}

int64_t getSystemTimeToBootTimeOffset() {
    return TimeBaseSingleton::get().getSystemTimeToBootTimeOffset();
}

int64_t getBootTimeToSystemTimeOffset() {
    return TimeBaseSingleton::get().getBootTimeToSystemTimeOffset();
}

void updateSystemTimeToBootTimeOffset() {
    return TimeBaseSingleton::get().updateSystemTimeToBootTimeOffset();
}

} // namespace android::audio_utils
