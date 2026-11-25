/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "ExtCamPrvdr"
// #define LOG_NDEBUG 0

#include "ExternalCameraProvider.h"

#include <ExternalCameraDevice.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <convert.h>
#include <cutils/properties.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <sys/inotify.h>
#include <regex>

namespace android {
namespace hardware {
namespace camera {
namespace provider {
namespace implementation {

using ::aidl::android::hardware::camera::common::Status;
using ::android::hardware::camera::device::implementation::ExternalCameraDevice;
using ::android::hardware::camera::device::implementation::fromStatus;
using ::android::hardware::camera::external::common::ExternalCameraConfig;

namespace {
// "device@<version>/external/<id>"
const std::regex kDeviceNameRE("device@([0-9]+\\.[0-9]+)/external/(.+)");

constexpr char kDevicePath[] = "/dev/";
constexpr char kPrefix[] = "video";
constexpr int kPrefixLen = sizeof(kPrefix) - 1;
constexpr int kDevicePrefixLen = sizeof(kDevicePath) + kPrefixLen - 1;

bool matchDeviceName(int cameraIdOffset, const std::string& deviceName, std::string* deviceVersion,
                     std::string* cameraDevicePath) {
    std::smatch sm;
    if (std::regex_match(deviceName, sm, kDeviceNameRE)) {
        if (deviceVersion != nullptr) {
            *deviceVersion = sm[1];
        }
        if (cameraDevicePath != nullptr) {
            *cameraDevicePath = "/dev/video" + std::to_string(std::stoi(sm[2]) - cameraIdOffset);
        }
        return true;
    }
    return false;
}
}  // namespace

ExternalCameraProvider::ExternalCameraProvider() : mCfg(ExternalCameraConfig::loadFromCfg()) {
    mHotPlugThread = std::make_shared<HotplugThread>(this);
    mHotPlugThread->run();
}

ExternalCameraProvider::~ExternalCameraProvider() {
    mHotPlugThread->requestExitAndWait();
}

ndk::ScopedAStatus ExternalCameraProvider::setCallback(
        const std::shared_ptr<ICameraProviderCallback>& in_callback) {
    if (in_callback == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }

    {
        Mutex::Autolock _l(mLock);
        mCallback = in_callback;
    }

    std::unordered_map<std::string, CameraDeviceStatus> snapshot;
    { Mutex::Autolock lock(mLock); snapshot = mCameraStatusMap; }
    for (const auto& pair : snapshot) {
        auto status = in_callback->cameraDeviceStatusChange(pair.first, pair.second);
        if (!status.isOk()) ALOGE("camera status replay failed: %s", status.getDescription().c_str());
    }
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getVendorTags(
        std::vector<VendorTagSection>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // No vendor tag support for USB camera
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getCameraIdList(std::vector<std::string>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // External camera HAL always report 0 camera, and extra cameras
    // are just reported via cameraDeviceStatusChange callbacks
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getCameraDeviceInterface(
        const std::string& in_cameraDeviceName, std::shared_ptr<ICameraDevice>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    std::string cameraDevicePath, deviceVersion;
    bool match = matchDeviceName(mCfg.cameraIdOffset, in_cameraDeviceName, &deviceVersion,
                                 &cameraDevicePath);

    if (!match) {
        *_aidl_return = nullptr;
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }

    Mutex::Autolock lock(mLock);
    if (mCameraStatusMap.count(in_cameraDeviceName) == 0 ||
        mCameraStatusMap[in_cameraDeviceName] != CameraDeviceStatus::PRESENT) {
        *_aidl_return = nullptr;
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }

    ALOGV("Constructing external camera device");
    std::shared_ptr<ExternalCameraDevice> deviceImpl =
            ndk::SharedRefBase::make<ExternalCameraDevice>(cameraDevicePath, mCfg);
    if (deviceImpl == nullptr || deviceImpl->isInitFailed()) {
        ALOGE("%s: camera device %s init failed!", __FUNCTION__, cameraDevicePath.c_str());
        *_aidl_return = nullptr;
        return fromStatus(Status::INTERNAL_ERROR);
    }

    IF_ALOGV() {
        int interfaceVersion;
        deviceImpl->getInterfaceVersion(&interfaceVersion);
        ALOGV("%s: device interface version: %d", __FUNCTION__, interfaceVersion);
    }

    *_aidl_return = deviceImpl;
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::notifyDeviceStateChange(int64_t) {
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::isConcurrentStreamCombinationSupported(
        const std::vector<CameraIdAndStreamCombination>&, bool* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // No concurrent stream combinations are supported
    *_aidl_return = false;
    return fromStatus(Status::OK);
}

void ExternalCameraProvider::addExternalCamera(const char* devName) {
    std::string deviceName = std::string("device@") + ExternalCameraDevice::kDeviceVersion + "/external/" +
        std::to_string(mCfg.cameraIdOffset + std::atoi(devName + kDevicePrefixLen));
    std::shared_ptr<ICameraProviderCallback> callback;
    { Mutex::Autolock lock(mLock); mCameraStatusMap[deviceName] = CameraDeviceStatus::PRESENT; callback = mCallback; }
    if (callback) {
        auto status = callback->cameraDeviceStatusChange(deviceName, CameraDeviceStatus::PRESENT);
        if (!status.isOk()) ALOGE("camera add callback failed: %s", status.getDescription().c_str());
    }
}

void ExternalCameraProvider::deviceAdded(const char* devName) {
    auto device = ndk::SharedRefBase::make<ExternalCameraDevice>(devName, mCfg);
    if (!device || device->isInitFailed()) {
        ALOGE("PipeWire camera initialization failed path=%s", devName); return;
    }
    addExternalCamera(devName);
}
void ExternalCameraProvider::deviceRemoved(const char* devName) {
    std::string name = std::string("device@") + ExternalCameraDevice::kDeviceVersion + "/external/" +
        std::to_string(mCfg.cameraIdOffset + std::atoi(devName + kDevicePrefixLen));
    std::shared_ptr<ICameraProviderCallback> callback;
    { Mutex::Autolock lock(mLock); if (!mCameraStatusMap.erase(name)) return; callback = mCallback; }
    if (callback) {
        auto status = callback->cameraDeviceStatusChange(name, CameraDeviceStatus::NOT_PRESENT);
        if (!status.isOk()) ALOGE("camera removal callback failed: %s", status.getDescription().c_str());
    }
}
void ExternalCameraProvider::updateAttachedCameras() {
    pwcam_device* devices = nullptr;
    size_t count = pwcam_devices(&devices);
    std::unordered_set<std::string> present;
    for (size_t i = 0; i < count; i++) {
        std::string path = "/dev/video" + std::to_string(devices[i].id);
        present.insert(path);
        if (mKnownDevices.count(path) && mGenerations[path] != devices[i].generation) {
            deviceRemoved(path.c_str());
            mKnownDevices.erase(path);
        }
        if (!mKnownDevices.count(path)) deviceAdded(path.c_str());
        mGenerations[path] = devices[i].generation;
    }
    free(devices);
    for (const auto& path : mKnownDevices) if (!present.count(path)) {
        deviceRemoved(path.c_str()); mGenerations.erase(path);
    }
    std::unordered_set<std::string> initialized;
    { Mutex::Autolock lock(mLock);
      for (const auto& path : present) {
          std::string name = std::string("device@") + ExternalCameraDevice::kDeviceVersion + "/external/" +
              std::to_string(mCfg.cameraIdOffset + std::atoi(path.c_str() + kDevicePrefixLen));
          if (mCameraStatusMap.count(name)) initialized.insert(path);
      }
    }
    mKnownDevices = std::move(initialized);
}
ExternalCameraProvider::HotplugThread::HotplugThread(ExternalCameraProvider* parent)
    : mParent(parent) {}
ExternalCameraProvider::HotplugThread::~HotplugThread() = default;
bool ExternalCameraProvider::HotplugThread::initialize() { return pwcam_start() == 0; }
bool ExternalCameraProvider::HotplugThread::threadLoop() {
    mParent->updateAttachedCameras();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return true;
}

// End ExternalCameraProvider::HotplugThread functions

}  // namespace implementation
}  // namespace provider
}  // namespace camera
}  // namespace hardware
}  // namespace android
