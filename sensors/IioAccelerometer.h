/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <aidl/android/hardware/sensors/BnSensors.h>
#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace matonos::sensors {
class Sensors;

class IioAccelerometer {
  public:
    struct Device {
        std::string path;
        double scale[3];
        double offset[3];
        double matrix[3][3];
    };
    static std::unique_ptr<IioAccelerometer> discover(Sensors*);
    ~IioAccelerometer();
    const aidl::android::hardware::sensors::SensorInfo& info() const { return mInfo; }
    void activate(bool);
    void batch(int64_t periodNs);
    ndk::ScopedAStatus flush();

  private:
    IioAccelerometer(Sensors*, Device);
    bool sample(float values[3]);
    void run();

    Sensors* mOwner;
    Device mDevice;
    aidl::android::hardware::sensors::SensorInfo mInfo;
    std::atomic<bool> mStop{false};
    bool mEnabled = false;
    int64_t mPeriodNs = 20000000;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::thread mThread;
};
}  // namespace matonos::sensors
