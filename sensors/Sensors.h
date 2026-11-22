/*
 * Copyright (C) 2021 The Android Open Source Project
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
 *
 * AIDL service structure adapted from hardware/interfaces/sensors/aidl/default.
 */
#pragma once

#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/sensors/BnSensors.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>
#include <memory>
#include <mutex>
#include <vector>

#include "IioAccelerometer.h"

namespace matonos::sensors {
using namespace aidl::android::hardware::sensors;
using Fmq = android::AidlMessageQueue<Event,
        aidl::android::hardware::common::fmq::SynchronizedReadWrite>;

class Sensors final : public BnSensors {
  public:
    Sensors();
    ~Sensors() override;

    ndk::ScopedAStatus activate(int32_t, bool) override;
    ndk::ScopedAStatus batch(int32_t, int64_t, int64_t) override;
    ndk::ScopedAStatus configDirectReport(int32_t, int32_t, ISensors::RateLevel, int32_t*) override;
    ndk::ScopedAStatus flush(int32_t) override;
    ndk::ScopedAStatus getSensorsList(std::vector<SensorInfo>*) override;
    ndk::ScopedAStatus initialize(
            const aidl::android::hardware::common::fmq::MQDescriptor<Event,
                    aidl::android::hardware::common::fmq::SynchronizedReadWrite>&,
            const aidl::android::hardware::common::fmq::MQDescriptor<int32_t,
                    aidl::android::hardware::common::fmq::SynchronizedReadWrite>&,
            const std::shared_ptr<ISensorsCallback>&) override;
    ndk::ScopedAStatus injectSensorData(const Event&) override;
    ndk::ScopedAStatus registerDirectChannel(const ISensors::SharedMemInfo&, int32_t*) override;
    ndk::ScopedAStatus setOperationMode(ISensors::OperationMode) override;
    ndk::ScopedAStatus unregisterDirectChannel(int32_t) override;

    void post(const std::vector<Event>& events);

  private:
    std::unique_ptr<IioAccelerometer> mAccelerometer;
    std::unique_ptr<Fmq> mEventQueue;
    android::hardware::EventFlag* mEventFlag = nullptr;
    std::mutex mQueueMutex;
};
}  // namespace matonos::sensors
