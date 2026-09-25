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

/**
 * @file safe_chrono.h
 * @brief This file provides utility functions for safe arithmetic operations on
 *        std::chrono time points and durations, preventing overflow.
 */

#pragma once

#include <chrono>
#include "safe_math.h"

namespace android::audio_utils {

/**
 * @brief Checks if adding a duration to a time point would result
 * in an overflow or underflow.
 *
 * This function determines if the sum of a chrono time point and a duration
 * would exceed the maximum representable value or fall below the minimum
 * representable value of the time point's type.
 *
 * @tparam T The type of the chrono time point (e.g., std::chrono::time_point).
 * @tparam D The type of the chrono duration (e.g., std::chrono::duration).
 * @param time The initial chrono time point.
 * @param duration The duration to be added.
 * @return True if the addition would overflow or underflow, false otherwise.
 */
template <typename T, typename D>
requires requires(T t, D d) { D::zero(); T::min(); T::max(); t + d; }
constexpr bool add_would_overflow(T time, D duration) {
    if (duration > D::zero()) {
        return time > T::max() - duration;
    } else if (duration < D::zero()) {
        return time < T::min() - duration;
    }
    return false;
}

/**
 * @brief Safely adds a duration to a time point, saturating at
 * the max/min value on overflow/underflow.
 *
 * This function adds a duration to a chrono time point. If the addition would
 * result in an overflow, the function returns the maximum representable value
 * for the time point's type. If it would result in an underflow, it returns
 * the minimum representable value.
 *
 * @tparam T The type of the chrono time point (e.g., std::chrono::time_point).
 * @tparam D The type of the chrono duration (e.g., std::chrono::duration).
 * @param time The initial chrono time point.
 * @param duration The duration to be added.
 * @return The result of the addition, or the min/max value of T if underflow/overflow occurred.
 */
template <typename T, typename D>
requires requires(T t, D d) { D::zero(); T::min(); T::max(); t + d; }
constexpr auto safe_add_sat(T time, D duration) {
    if (duration > D::zero()) {
        if (time > T::max() - duration) {
            return T::max();
        }
    } else if (duration < D::zero()) {
        if (time < T::min() - duration) {
            return T::min();
        }
    }
    return time + duration;
}

} // namespace android::audio_utils
