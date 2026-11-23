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
 * FMQ and ISensors method structure adapted from
 * hardware/interfaces/sensors/aidl/default/Sensors.cpp.
 */
#include "Sensors.h"

#include <android-base/logging.h>
#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>

namespace matonos::sensors {
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using EventPayload = aidl::android::hardware::sensors::Event::EventPayload;
using ::android::status_t;

Sensors::Sensors() : mAccelerometer(IioAccelerometer::discover(this)) {
    if (mAccelerometer) {
        LOG(INFO) << "IIO accelerometer registered: " << mAccelerometer->info().name;
    } else {
        LOG(INFO) << "No usable IIO accelerometer found; registering empty sensor list";
    }
}

Sensors::~Sensors() {
    mAccelerometer.reset();
    if (mEventFlag != nullptr) {
        const status_t status = android::hardware::EventFlag::deleteEventFlag(&mEventFlag);
        if (status != android::OK) LOG(ERROR) << "deleteEventFlag failed: status=" << status;
    }
}

ndk::ScopedAStatus Sensors::activate(int32_t handle, bool enabled) {
    if (!mAccelerometer || handle != mAccelerometer->info().sensorHandle)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    mAccelerometer->activate(enabled);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Sensors::batch(int32_t handle, int64_t periodNs, int64_t) {
    if (!mAccelerometer || handle != mAccelerometer->info().sensorHandle)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    mAccelerometer->batch(periodNs);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Sensors::configDirectReport(int32_t, int32_t, ISensors::RateLevel, int32_t* out) {
    *out = EX_UNSUPPORTED_OPERATION;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Sensors::flush(int32_t handle) {
    if (!mAccelerometer || handle != mAccelerometer->info().sensorHandle)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    return mAccelerometer->flush();
}

ndk::ScopedAStatus Sensors::getSensorsList(std::vector<SensorInfo>* out) {
    if (mAccelerometer) out->push_back(mAccelerometer->info());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Sensors::initialize(
        const MQDescriptor<Event, SynchronizedReadWrite>& eventDescriptor,
        const MQDescriptor<int32_t, SynchronizedReadWrite>&,
        const std::shared_ptr<ISensorsCallback>&) {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    if (mEventFlag != nullptr) {
        const status_t status = android::hardware::EventFlag::deleteEventFlag(&mEventFlag);
        if (status != android::OK) LOG(ERROR) << "deleteEventFlag during initialize failed: status=" << status;
    }
    mEventQueue = std::make_unique<Fmq>(eventDescriptor, true);
    if (!mEventQueue->isValid()) {
        LOG(ERROR) << "Sensors event FMQ descriptor is invalid";
        mEventQueue.reset();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const status_t status = android::hardware::EventFlag::createEventFlag(
            mEventQueue->getEventFlagWord(), &mEventFlag);
    if (status != android::OK) {
        LOG(ERROR) << "createEventFlag failed: status=" << status;
        mEventQueue.reset();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Sensors::injectSensorData(const Event&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Sensors::registerDirectChannel(const ISensors::SharedMemInfo&, int32_t* out) {
    *out = EX_UNSUPPORTED_OPERATION;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Sensors::setOperationMode(ISensors::OperationMode mode) {
    if (mode != ISensors::OperationMode::NORMAL)
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Sensors::unregisterDirectChannel(int32_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

void Sensors::post(const std::vector<Event>& events) {
    if (events.empty()) return;
    std::lock_guard<std::mutex> lock(mQueueMutex);
    if (!mEventQueue || !mEventFlag) return;
    if (!mEventQueue->write(events.data(), events.size())) {
        LOG(ERROR) << "Sensors event FMQ full; dropped " << events.size() << " event(s)";
        return;
    }
    const status_t status = mEventFlag->wake(
            static_cast<uint32_t>(BnSensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS));
    if (status != android::OK) LOG(ERROR) << "Sensors event FMQ wake failed: status=" << status;
}
}  // namespace matonos::sensors
