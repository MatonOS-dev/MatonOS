# Bluetooth

MatonOS packages its fork of AOSP's generic `android.hardware.bluetooth-service.matonos`
Linux HCI AIDL HAL. The fork keeps AOSP's MGMT discovery and kernel HCI user
channel path, while moving controller discovery and setup to a worker. Binder
calls remain available during that wait, so close and client death cancel the
wait and return the HAL to READY without notifying a dead client. An initialize
received in an unexpected live state reports `ALREADY_INITIALIZED` and returns;
a stale ONE_CLIENT state whose client-death notice has arrived is cleaned up
before a fresh initialization.

With no controller, initialization waits for one to appear. A controller
hotplug therefore completes initialization without restarting the HAL. If the
controller is unplugged while active, the HCI socket disconnects; Android's
Bluetooth stack and HAL restart path can initialize again and wait for the next
controller. The service retains AOSP's `IBluetoothHci/default` instance, and
runs a one-thread Binder pool so close/death calls can run while setup is
waiting. No virtual controller is created. Keep `vendor.ser.bt-uart` unset so
the HAL follows the kernel HCI path.

## Bluetooth audio

The static audio policy includes AOSP's A2DP, hearing-aid, and LE Audio
device ports and routes. The audio core-v4 manifest declares the matching
`IModule/bluetooth`; the HAL builds that module from policy at startup. This
does not depend on Bluetooth being powered on or on an HCI controller being
present. When there is no controller, the Bluetooth setting defaults off and
no Bluetooth stack connection is established, so the policy module has no
connected device route while primary audio remains usable.

The audio HAL links AOSP's stock `android.hardware.bluetooth.audio-impl` and
registers `IBluetoothAudioProviderFactory/default` from the HAL process. The
provider and Bluetooth stack supply software A2DP encoding and LE Audio
encoding/decoding sessions. The ODM audio manifest declares the stock
`android.hardware.bluetooth.audio` provider instance because the BayLibre
APEX does not bundle AOSP's provider VINTF fragment. AOSP's stock SELinux
`service_contexts` maps this factory to `hal_audio_service`; the existing
`hal_audio` service policy permits the registration. No MatonOS init service,
HAL, or SELinux policy is added for audio.

The no-controller behavior is supported by the stock source path:

- `hardware/interfaces/audio/aidl/default/Module.cpp:114-141` maps the
  configured `bluetooth` module name to `ModuleBluetooth`.
- `hardware/interfaces/audio/aidl/default/bluetooth/ModuleBluetoothBase.cpp:78-128`
  creates the A2DP and LE profile interfaces without discovering an HCI
  controller; `getBluetooth()` only returns a SCO interface if a SCO port is
  configured.
- `hardware/interfaces/bluetooth/audio/aidl/default/Android.bp:10-29` defines
  the stock provider as a shared library with a VINTF fragment. Its
  `hardware/interfaces/bluetooth/audio/aidl/default/BluetoothAudioProviderFactory.cpp:52-70`
  creates the A2DP software and LE software providers.
- `system/sepolicy/private/service_contexts:45` labels the factory as
  `hal_audio_service`; `system/sepolicy/private/hal_audio.te:1-8` gives
  `hal_audio` the stock service registration policy.

The shipping configuration is in `audio/config-odm/` and the matching vendor
fallback is in `audio/config/`. Both declare the same `bluetooth` module. The
audio README records source and validation details.

The forked HAL runs in AOSP's `hal_bluetooth_default` domain. The shared fixed
MatonOS policy labels `/dev/rfkill` and grants that domain access to the node;
AOSP's existing Bluetooth policy grants its HCI socket access. The custom
HAL's former `matonos_driver` Bluetooth server/socket/property rules are gone.

## Verification

Run `MATON_BUILD_COORDINATOR=1 tools/preflight.sh` before requesting a full
image. The coordinator builds AOSP; do not run an AOSP build from this area.
After a fresh QEMU boot with no host Bluetooth passthrough:

1. Confirm `sys.boot_completed=1` and Bluetooth is unavailable/off.
2. Check `logcat -b crash -d` and `logcat -b all -d` for no repeated
   `com.android.bluetooth` aborts.
3. Open Developer options and confirm it stays open.
4. Confirm `service check android.hardware.bluetooth.IBluetoothHci/default`
   shows the MatonOS service. With no controller, confirm the service remains
   responsive while initialization waits; then hotplug a controller and check
   that initialization completes. Toggle Bluetooth off during a wait and
   confirm the stack can restart without repeated `ALREADY_INITIALIZED` aborts.

With a real Bluetooth controller attached to a test PC, fresh boot and
confirm `hci0` appears in `dumpsys bluetooth_manager`/`dmesg`, and
`IBluetoothHci/default` initializes without an HCI bind error. Then pair a
classic A2DP headset, select it as output, play the audio test track, and
confirm audible output. For LE Audio, repeat with an LE-capable controller and
peer and verify a BLE output route and an active software-encoded session.
QEMU test limitations and the proposed VM plan are documented in
`/mnt/data/aosp/out/pc-logs/agents/bt-audio.report.md`.

## Real hardware test sequence

1. On the Ryzen 5800X PC with Intel 7265 Wi-Fi+Bluetooth, confirm the stock
   HAL initializes `hci0`, then scan, pair a keyboard or mouse, verify HID,
   and transfer a small file.
2. On Surface Pro 3, confirm the Marvell 88W8897 controller and firmware
   initialize `hci0`, then scan and pair a device.
3. On HP ProDesk 600 G1 with no Bluetooth controller, confirm boot completes,
   Bluetooth stays unavailable/off, Developer options opens, and no Bluetooth
   process abort loop occurs. Confirm primary audio still plays and the
   Bluetooth audio module is registered without connected device routes.
4. On the build PC, pair an A2DP headset and verify normal media playback over
   Bluetooth. If an LE Audio headset is available, check its output and input
   routes separately.

## Dropped behavior

The empty VHCI emulator remains out of scope. Controller choice is handled by
`native/matonos-btd` and the bridge; the HAL binds the kernel's default controller interface.
The fork only makes discovery cancellable and setup asynchronous; it does not
proxy between multiple physical controllers.


## Controller choice (btd1)

`native/matonos-btd` lists HCI controllers using MGMT READ_INFO (including
powered-off controllers), and remembers USB identities before detaching btusb.
The bridge's `bluetooth` target is allowlisted for Settings, with the same
certificate/permission checks as `wifi`. `select_device` disables Bluetooth,
waits for OFF, asks btd to unbind other USB controllers and bind the chosen
one, then restores the previous enabled state. Non-USB competitors are logged
and left alone. No HAL transport or virtual controller is introduced.

The persisted key is `persist.vendor.maton.bluetooth.device` (BD address).
No key means no boot-time changes. A five-second init one-shot runs before
the HAL; failure/timeout proceeds with the HAL regardless. The long-running
daemon polls sysfs once per second and publishes `devices_changed`; the bridge
reconciles a reappearing preferred controller with Bluetooth powered down.
Cache: `/data/vendor/maton-btd/devices`, written before unbinding so detached
controllers remain selectable after a daemon restart. Unknown identities are
listed with an empty address and prevent destructive selection until readable.

NDK link and switcher javac checks pass locally. Preflight has only the six
existing missing APK inputs; policy/image/runtime tests remain unverified.
See `btd1.report.md` for exact commands and coordinator tests. On Ryzen/Intel
7265, Surface/Marvell and HP ProDesk (no controller), fresh-boot first, disable
sleepd idle suspend, list devices, select an address while on and off, verify
state restoration, and unplug/replug the preferred controller. Add two USB
adapters to test switching back to the cached unbound adapter; verify the
chosen address is the HAL's controller, pairing/HID still work, and no AVCs or
crash loop occurs. Non-USB competing controllers cannot be made the default
by USB unbind alone; this is reported rather than changing the HAL.
