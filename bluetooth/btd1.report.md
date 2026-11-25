# btd1 report

Implementation commit: d12abde5 on wip/btd. This report is committed separately.
No push, AOSP patch, HAL change, kernel change, Soong/Gradle build or VM run.
All writes were inside /home/hanro50/matonos/build/dt-btd.

## Files and IPC

- native/matonos-btd/{matonos-btd.cpp,CMakeLists.txt}: NDK daemon; automatically
  discovered by build-native.sh, linked to the existing IPC library.
- systembridge/src/org/matonos/systembridge/{BluetoothControllerSwitcher.java,
  SystemBridgeService.java}: serialized OFF/select/restore, subscription and
  preferred-controller hotplug reconciliation.
- systembridge/AndroidManifest.xml: BLUETOOTH_CONNECT and BLUETOOTH_PRIVILEGED.
  buildinfra/privapp-permissions/privapp-permissions-org.matonos.systembridge.xml
  grants the latter. BluetoothAdapter.enable()/disable() are deliberately used
  by the platform-signed system bridge, exempt from public-app restrictions.
- systembridge/res/raw/target_caller_allowlist.txt: bluetooth target for Settings,
  behind existing certificate, permission, target and audit checks.
- radio/apex/{payload.list,matonos-radio.rc,file_contexts}: packaging and startup.
- Existing fixed sepolicy/matonos/{matonos_wifid.te,matonos_driver.te,
  file_contexts,genfs_contexts,service_contexts}: policy only, no new files.
- bluetooth/README.md: design, limitations and real-PC test sequence.

The bluetooth instance was already declared in bundle/matonos-channel.xml.
Channel: vendor.matonos.channel.IChannel/bluetooth. Wi-Fi-compatible JSON shapes:

```
list_devices {} -> {"devices":[{"address":"AA:BB:CC:DD:EE:FF",
  "perm_mac":"AA:BB:CC:DD:EE:FF","interface":"hci0","device":"/sys/devices/...",
  "bus":"USB","usb_id":"8087:0a2a","name":"...","bound":true,"selected":false}]}
select_device {"device":"AA:BB:CC:DD:EE:FF","persist":true}
  -> {"selected":"AA:BB:CC:DD:EE:FF"}
get_state {} -> {"selected":"AA:BB:CC:DD:EE:FF"}
```

persist defaults true; false selects only this session. Bridge explicit choices
persist. devices_changed publishes a list_devices snapshot when enumeration
changes (one-second polling). Bridge subscribes on startup/reconnect and applies
a preferred controller if it appears unbound or another USB controller appears.
It records ON/TURNING_ON, disables, waits for STATE_OFF, selects, and restores
ON only when previously requested on, even after selection failure. No choice
means no policy change. Unknown identities are listed with an empty address;
selection refuses to detach anything until all bound identities are known.

## Interfaces, policy and boot

/sys/class/bluetooth/hci*/device gives physical ancestry and HCI index. MGMT
READ_INFO (opcode 4, AF_BLUETOOTH/BTPROTO_HCI control channel 3) reads public BD
address/name with Powered clear; each request has a 500 ms deadline. USB
identifiers alone cannot reliably give the address. Driver ancestry identifies
btusb and its interface name. Parent USB idVendor/idProduct and devnum give
metadata and attachment generation.

/sys/bus/usb/drivers/btusb/unbind detaches every other USB controller;
/sys/bus/usb/drivers/btusb/bind attaches the chosen one when needed. Only
kernel-discovered interface names are written. Non-USB competitors are logged
and left alone; their lower index may still win in the unchanged HAL.
/sys/bus/usb/devices/<interface> plus parent devnum rejects stale unbound cache
entries when an adapter is unplugged/replaced in the same USB port.

persist.vendor.maton.bluetooth.device stores the preferred address.
/data/vendor/maton-btd/devices caches address/interface/VID/PID/devnum/path
before detach, through devices.tmp and atomic rename, so unbound controllers
remain selectable across daemon restart. Unbound names after restart are absent.
Bound devices are rediscovered at boot; USB paths can change between boots.

Policy reasons: separate matonos_btd domain in the existing Wi-Fi policy file
avoids the HAL-server net_admin neverallow, as wifid does. MGMT needs net_admin
and Bluetooth socket create/bind/read/write. Binder service registration and
bridge callbacks use the existing channel type; its registration neverallow
explicitly admits btd. vendor_maton_prop get/set permits persisted choice.
Generic sysfs is read/search only. A new sysfs_maton_btusb type labels only the
two driver binding files and allows open/write/getattr. Dedicated vendor cache
dir/file permissions allow persistence without platform Bluetooth data access.
Reviewed private/domain.te and private/hal_neverallows.te: no generic ban on
these specifically labeled vendor sysfs binding writes; usermodehelper and HAL
capability bans do not apply. Actual policy compilation remains unverified.

apex.all.ready creates the cache directory, then exec_start runs btd --boot
with timeout_period 5 before asynchronously starting daemon and HAL. No choice
returns immediately. Selection failure, missing binary or timeout continues
the init action; no daemon-ready property gates the HAL or boot. IPC startup
failure exits loudly; HAL never depends on the channel being registered.

## Checks run

Working directory was the worktree; .btd-check scratch was removed afterwards.

```sh
NDK=/home/hanro50/Android/Sdk/ndk/30.0.16248370
CMAKE=/mnt/data/aosp/prebuilts/cmake/linux-x86/bin/cmake
"$CMAKE" -S native/matonos-btd -B .btd-check/cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-35 \
  -DMATON_IPC_INCLUDE_DIR="$PWD/native/libmatonos-ipc/include" \
  -DMATON_IPC_LIBRARY=/mnt/data/aosp/out/pc-native/libmatonos-ipc/libmatonos-ipc.so
"$CMAKE" --build .btd-check/cmake --parallel "$(nproc)"
javac -Xlint:all -Werror -d .btd-check/java \
  -cp /home/hanro50/Android/Sdk/platforms/android-36/android.jar:/mnt/data/aosp/out/soong/.intermediates/frameworks/base/framework-minus-apex/android_common/combined/framework.jar:/mnt/data/aosp/out/soong/.intermediates/device/maton/pc_x86_64/buildinfra/vendor.matonos.channel-V1-java/android_common/javac/vendor.matonos.channel-V1-java.jar \
  systembridge/src/org/matonos/systembridge/BluetoothControllerSwitcher.java
PATH=/mnt/data/aosp/out/host/linux-x86/bin:$PATH MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh
git diff --check
```

PASS: actual CMake NDK compile/link, -Wall -Wextra -Werror; also direct NDK
syntax/link checks. cmake was absent on PATH; the checkout prebuilt worked.
PASS: switcher javac after scoped deprecation suppression for privileged toggles.
Full service/platform bridge compilation still needs the coordinator build.
PASS: diff whitespace. Preflight FAIL: only six pre-existing missing APK inputs:
subsystem bootstrap, Fennec, Thunderbird, MicroGmsCore, MicroGCompanion and
MicroGGsfProxy. Full build-native/build-apps were not run because this task
prohibits shared writes and Soong/Gradle; isolated native recipe compiled instead.

## Controller-less HAL source review

BluetoothHci initializes asynchronously and cancels the MGMT wait on close or
callback death, clearing the prior session for retries. This addresses the
known Binder stall and stale-session retry path. However waitHciDev still waits
indefinitely when no controller exists, rather than immediately returning
UNABLE_TO_OPEN_INTERFACE. Source review cannot establish that every framework
controller-less timeout/crash is fixed: fresh-image ON/OFF/retry tests remain
required. No HAL change was made.

## Coordinator tests

Restore normal prebuilt APK inputs; build native recipe, radio APEX and full
image, compile policy/neverallows, run check-selinux-labels.sh and VINTF/APEX
validation. Fresh image only, no hot patches. Use a free agent port, never 5555:

```sh
tools/run-qemu-live.sh -g none -m 4096 -a 5556 -s <logs>/btd1-none.log
ADB=/mnt/data/aosp/out/host/linux-x86/bin/adb
"$ADB" -s 127.0.0.1:5556 wait-for-device
"$ADB" -s 127.0.0.1:5556 root
"$ADB" -s 127.0.0.1:5556 shell setprop persist.vendor.maton.sleep_idle_s 0
"$ADB" -s 127.0.0.1:5556 shell getprop sys.boot_completed
"$ADB" -s 127.0.0.1:5556 shell dumpsys bluetooth_manager
"$ADB" -s 127.0.0.1:5556 logcat -b crash -d
"$ADB" -s 127.0.0.1:5556 logcat -b all -d -s matonos-btd MatonSystemBridge android.hardware.bluetooth.service.matonos
"$ADB" -s 127.0.0.1:5556 shell svc bluetooth enable
"$ADB" -s 127.0.0.1:5556 shell svc bluetooth disable
```

Require boot completed, empty list through an authenticated bridge test client,
responsive Developer options/HAL/IPC, no abort loop, repeated OFF/ON while no
controller exists. Repeat with a stale persisted address and a separately built
image with missing/failing btd; HAL/boot must still start after bounded timeout.
Live RAM /data resets on reboot: start a fresh VM per image and use an installed
persistent-data image for reboot persistence tests.

One-controller test, coordinator only, with the task-authorized host controller
8087:0a2a available and not in active host use:

```sh
tools/run-qemu-live.sh -g none -m 4096 -a 5556 -s <logs>/btd1-intel.log \
  -x '-device usb-host,id=btd1bt,vendorid=0x8087,productid=0x0a2a'
```

Use the normal harness USB controller. Require correct address, USB/8087:0a2a,
path/bound/name. Authenticated Settings/test client: call bluetooth/list_devices,
subscribe devices_changed, select_device with the discovered address while ON
and OFF; verify OFF precedes sysfs writes and ON restoration only when initially
ON. Check property, pairing/HID, AVCs and crash logs. QMP device_del btd1bt and
device_add the same usb-host options exercise remove/replug and topic/selection.

Two USB adapters are unavailable in QEMU: real hardware must test unbinding
competitors, switching back to cached unbound devices after daemon restart,
identical-model replacement, composite btusb interfaces, rfkill and selected
lowest/only HCI index. Non-USB competitors must be logged and left untouched;
verify/document the unchanged HAL's chosen controller. Run README's Ryzen/Intel
7265, Surface/Marvell 88W8897 and HP ProDesk/no-controller tests.

Unverified: all runtime/VM/hardware tests, full bridge build and runtime grants,
SELinux compilation/AVCs, actual sysfs labels on generic PCs, APEX/VINTF packing,
MGMT READ_INFO while the HAL owns the HCI user channel, multi-interface btusb
binding behavior, hotplug/restart and manual Bluetooth-toggle races. No Settings
UI was added. No pending SHARED-CHANGES, kernel change or AOSP patch is needed.
