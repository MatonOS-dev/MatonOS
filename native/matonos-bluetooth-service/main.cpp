/* SPDX-License-Identifier: Apache-2.0 */
#include "BluetoothHci.h"
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(4);
  auto service = ndk::SharedRefBase::make<maton::BluetoothHci>();
  const char* name = "android.hardware.bluetooth.IBluetoothHci/default";
  binder_status_t status = AServiceManager_addService(service->asBinder().get(), name);
  if (status != STATUS_OK) {
    __android_log_print(ANDROID_LOG_ERROR, "MatonBluetoothHci", "register service failed: %d", status);
    return 1;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}
