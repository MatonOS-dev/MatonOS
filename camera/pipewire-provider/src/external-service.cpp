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

#include <ExternalCameraProvider.h>
#include <unistd.h>
#include <cerrno>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using ::android::hardware::camera::provider::implementation::ExternalCameraProvider;

int main() {
    ALOGI("CameraProvider: external webcam service is starting.");

    long workers = sysconf(_SC_NPROCESSORS_ONLN);
    if (workers <= 0) { ALOGE("CPU count unavailable errno=%d", errno); return EXIT_FAILURE; }
    if (!ABinderProcess_setThreadPoolMaxThreadCount(static_cast<uint32_t>(workers))) {
        ALOGE("camera Binder thread pool setup failed workers=%ld", workers); return EXIT_FAILURE;
    }

    std::shared_ptr<ExternalCameraProvider> defaultProvider =
            ndk::SharedRefBase::make<ExternalCameraProvider>();
    const std::string serviceName = std::string(ExternalCameraProvider::descriptor) + "/external/0";

#ifdef LAZY_SERVICE
    binder_exception_t ret = AServiceManager_registerLazyService(defaultProvider->asBinder().get(),
                                                                 serviceName.c_str());
    LOG_ALWAYS_FATAL_IF(ret != EX_NONE,
                        "Error while registering lazy ext camera provider service: %d", ret);
#else
    binder_exception_t ret =
            AServiceManager_addService(defaultProvider->asBinder().get(), serviceName.c_str());
    LOG_ALWAYS_FATAL_IF(ret != EX_NONE, "Error while registering ext camera provider service: %d",
                        ret);
#endif

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}