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

#pragma once

namespace android::audio_utils::property {

/**
 * Check if running on a relaxed timing device, such as Cuttlefish, GSI, or Emulator.
 *
 * <p>Such a device is not an actual shipping configuration, but one where functional
 * compatibility is being tested instead of performance.
 *
 * @return true if a relaxed timing device.
 */
bool isRelaxedTimingDevice();

}  // namespace android::audio_utils::property
