/* SPDX-License-Identifier: Apache-2.0 */
#include "IioAccelerometer.h"
#include "Sensors.h"

#include <android-base/logging.h>
#include <aidl/android/hardware/sensors/BnSensors.h>
#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/SensorStatus.h>
#include <aidl/android/hardware/sensors/SensorType.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string_view>
#include <time.h>
#include <utility>
#include <unistd.h>

namespace matonos::sensors {
namespace {
using EventPayload = aidl::android::hardware::sensors::Event::EventPayload;
constexpr char kIioRoot[] = "/sys/bus/iio/devices";
constexpr int64_t kMinPeriodNs = 10000000;
constexpr int64_t kMaxPeriodNs = 1000000000;
constexpr double kGravity = 9.80665;

bool readText(const std::string& path, std::string* value) {
    std::ifstream stream(path);
    if (!stream) return false;
    std::getline(stream, *value);
    return !stream.bad();
}

bool readNumber(const std::string& path, double* value) {
    std::string text;
    if (!readText(path, &text)) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (errno || end == text.c_str() || !end || *end != '\0' || !std::isfinite(parsed)) {
        if (errno == 0) errno = EINVAL;
        return false;
    }
    *value = parsed;
    return true;
}

int deviceIndex(const std::string& name) {
    constexpr std::string_view prefix("iio:device");
    if (name.compare(0, prefix.size(), prefix) != 0) return -1;
    int index = -1;
    const char* first = name.data() + prefix.size();
    const char* last = name.data() + name.size();
    const auto result = std::from_chars(first, last, index);
    return result.ec == std::errc() && result.ptr == last ? index : -1;
}

bool readMatrix(const std::string& path, double matrix[3][3]) {
    std::string text;
    if (!readText(path, &text)) return false;
    for (char& c : text) if (c == ',' || c == ';' || c == '[' || c == ']' || c == '(' || c == ')') c = ' ';
    std::istringstream input(text);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (!(input >> matrix[row][col]) || !std::isfinite(matrix[row][col])) return false;
        }
    }
    std::string extra;
    return !(input >> extra);
}

bool readOptionalNumber(const std::string& path, double* value) {
    errno = 0;
    if (access(path.c_str(), F_OK) != 0) {
        if (errno != ENOENT && errno != ENOTDIR)
            PLOG(ERROR) << "access optional IIO attribute failed path=" << path;
        return false;
    }
    if (!readNumber(path, value)) {
        PLOG(ERROR) << "parse optional IIO numeric attribute failed path=" << path;
        return false;
    }
    return true;
}

bool readOptionalMatrix(const std::string& path, double matrix[3][3]) {
    errno = 0;
    if (access(path.c_str(), F_OK) != 0) {
        if (errno != ENOENT && errno != ENOTDIR)
            PLOG(ERROR) << "access optional IIO mount matrix failed path=" << path;
        return false;
    }
    if (!readMatrix(path, matrix)) {
        errno = EINVAL;
        PLOG(ERROR) << "parse IIO mount matrix failed path=" << path;
        return false;
    }
    return true;
}

bool discoverDevice(IioAccelerometer::Device* device) {
    DIR* dir = opendir(kIioRoot);
    if (!dir) {
        if (errno != ENOENT && errno != ENOTDIR)
            PLOG(ERROR) << "opendir IIO device root failed path=" << kIioRoot;
        return false;
    }
    struct Candidate { int index; std::string entry; bool accelName; };
    std::vector<Candidate> candidates;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) {
            if (errno != 0) PLOG(ERROR) << "readdir IIO device root failed path=" << kIioRoot;
            break;
        }
        const int index = deviceIndex(entry->d_name);
        if (index >= 0) {
            std::string sensorName;
            const std::string base = std::string(kIioRoot) + "/" + entry->d_name;
            readText(base + "/name", &sensorName);
            candidates.push_back({index, entry->d_name,
                    sensorName.find("accel") != std::string::npos ||
                    sensorName.find("Accel") != std::string::npos});
        }
    }
    if (closedir(dir) != 0) PLOG(ERROR) << "closedir IIO device root failed path=" << kIioRoot;
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.accelName != b.accelName) return a.accelName;
        return a.index < b.index;
    });

    const char* axes[] = {"x", "y", "z"};
    for (const auto& candidateInfo : candidates) {
        const int index = candidateInfo.index;
        const std::string& name = candidateInfo.entry;
        const std::string base = std::string(kIioRoot) + "/" + name;
        IioAccelerometer::Device candidate{};
        candidate.path = base;
        bool valid = true;
        for (int axis = 0; axis < 3; ++axis) {
            const std::string stem = base + "/in_accel_" + axes[axis];
            double rawProbe;
            if (!readNumber(stem + "_raw", &rawProbe)) { valid = false; break; }
            if (!readOptionalNumber(stem + "_scale", &candidate.scale[axis])) {
                // Several IIO drivers publish a common scale instead of per-axis values.
                if (!readOptionalNumber(base + "/in_accel_scale", &candidate.scale[axis])) {
                    PLOG(ERROR) << "IIO accelerometer scale unavailable path=" << stem
                                << "_scale or " << base << "/in_accel_scale"
                                << " (device index=" << index << ")";
                    valid = false;
                    break;
                }
            }
            if (!readOptionalNumber(stem + "_offset", &candidate.offset[axis]) &&
                !readOptionalNumber(base + "/in_accel_offset", &candidate.offset[axis])) {
                candidate.offset[axis] = 0;
            }
        }
        if (!valid) continue;
        const std::string matrixPath = base + "/in_mount_matrix";
        const std::string altMatrixPath = base + "/in_accel_mount_matrix";
        if (!readOptionalMatrix(matrixPath, candidate.matrix) &&
            !readOptionalMatrix(altMatrixPath, candidate.matrix)) {
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) candidate.matrix[i][j] = (i == j) ? 1 : 0;
        }
        *device = std::move(candidate);
        LOG(INFO) << "Selected IIO accelerometer device index=" << index << " name=" << name;
        return true;
    }
    return false;
}
}  // namespace

std::unique_ptr<IioAccelerometer> IioAccelerometer::discover(Sensors* owner) {
    Device device{};
    if (!discoverDevice(&device)) return nullptr;
    return std::unique_ptr<IioAccelerometer>(new IioAccelerometer(owner, std::move(device)));
}

IioAccelerometer::IioAccelerometer(Sensors* owner, Device device)
    : mOwner(owner), mDevice(std::move(device)) {
    mInfo.sensorHandle = 1;
    mInfo.name = "MatonOS IIO Accelerometer";
    mInfo.vendor = "Linux IIO";
    mInfo.version = 1;
    mInfo.type = aidl::android::hardware::sensors::SensorType::ACCELEROMETER;
    mInfo.typeAsString = "";
    mInfo.maxRange = 8.0f * static_cast<float>(kGravity);
    mInfo.resolution = 0.001f;
    mInfo.power = 0.1f;
    mInfo.minDelayUs = 10000;
    mInfo.maxDelayUs = 1000000;
    mInfo.fifoReservedEventCount = 0;
    mInfo.fifoMaxEventCount = 0;
    mInfo.requiredPermission = "";
    mInfo.flags = 0;
    mThread = std::thread(&IioAccelerometer::run, this);
}

IioAccelerometer::~IioAccelerometer() {
    mStop = true;
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void IioAccelerometer::activate(bool enabled) {
    std::lock_guard<std::mutex> lock(mMutex);
    mEnabled = enabled;
    mCv.notify_all();
}

void IioAccelerometer::batch(int64_t periodNs) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPeriodNs = std::clamp(periodNs, kMinPeriodNs, kMaxPeriodNs);
    mCv.notify_all();
}

ndk::ScopedAStatus IioAccelerometer::flush() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mEnabled) return ndk::ScopedAStatus::fromServiceSpecificError(BnSensors::ERROR_BAD_VALUE);
    aidl::android::hardware::sensors::Event event;
    event.sensorHandle = mInfo.sensorHandle;
    event.sensorType = aidl::android::hardware::sensors::SensorType::META_DATA;
    EventPayload::MetaData metadata{};
    metadata.what = EventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE;
    event.payload.set<EventPayload::Tag::meta>(metadata);
    mOwner->post({event});
    return ndk::ScopedAStatus::ok();
}

bool IioAccelerometer::sample(float out[3]) {
    const char* axes[] = {"x", "y", "z"};
    double raw[3];
    for (int axis = 0; axis < 3; ++axis) {
        const std::string path = mDevice.path + "/in_accel_" + axes[axis] + "_raw";
        if (!readNumber(path, &raw[axis])) {
            PLOG(ERROR) << "read IIO accelerometer sample failed path=" << path;
            return false;
        }
    }
    double scaled[3];
    for (int axis = 0; axis < 3; ++axis)
        scaled[axis] = (raw[axis] + mDevice.offset[axis]) * mDevice.scale[axis];
    for (int row = 0; row < 3; ++row) {
        out[row] = 0;
        for (int col = 0; col < 3; ++col)
            out[row] += static_cast<float>(mDevice.matrix[row][col] * scaled[col]);
    }
    return true;
}

void IioAccelerometer::run() {
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStop) {
        mCv.wait(lock, [&] { return mStop || mEnabled; });
        if (mStop) break;
        const int64_t period = mPeriodNs;
        lock.unlock();
        float values[3];
        if (sample(values)) {
            timespec now{};
            if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) {
                PLOG(ERROR) << "clock_gettime(CLOCK_BOOTTIME) failed for IIO event";
                lock.lock();
                mEnabled = false;
                continue;
            } else {
                aidl::android::hardware::sensors::Event event;
                event.sensorHandle = mInfo.sensorHandle;
                event.sensorType = mInfo.type;
                event.timestamp = static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
                EventPayload::Vec3 vector{};
                vector.x = values[0];
                vector.y = values[1];
                vector.z = values[2];
                vector.status = aidl::android::hardware::sensors::SensorStatus::ACCURACY_HIGH;
                event.payload.set<EventPayload::Tag::vec3>(vector);
                mOwner->post({event});
            }
        } else {
            // A discovered sysfs node disappearing or becoming unreadable is
            // unexpected. Report once, then leave the HAL alive and quiet.
            lock.lock();
            mEnabled = false;
            continue;
        }
        lock.lock();
        mCv.wait_for(lock, std::chrono::nanoseconds(period));
    }
}
}  // namespace matonos::sensors
