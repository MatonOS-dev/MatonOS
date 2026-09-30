#include "BootControl.h"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<matonos::bootctrl::BootControl>();
    constexpr char kInstance[] = "android.hardware.boot.IBootControl/default";
    const binder_status_t status = AServiceManager_addService(service->asBinder().get(), kInstance);
    if (status != STATUS_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "MatonosBootControl",
                            "BootControl registration failed: %d", status);
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 0;
}
