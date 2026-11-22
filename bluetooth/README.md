# Bluetooth

MatonOS packages AOSP's stock `android.hardware.bluetooth-service.default`
Linux HCI AIDL HAL. The HAL uses the kernel's Bluetooth management channel to
unblock rfkill and discover the controller, then opens the standard HCI user
channel. It registers the stable `IBluetoothHci/default` instance for the
Android Bluetooth stack.

With no controller, the stock HAL reports initialization failure and the
Bluetooth stack aborts on it (`hci_backend_aidl.cc` checks for SUCCESS). The
SettingsProvider overlay therefore defaults `bluetooth_on` to off, so boot does
not enable the stack; a user toggle on a controller-less PC still crashes it. No virtual controller is
created. Controller selection, hotplug replacement, and the MatonOS Bluetooth
adapter picker are dropped; the stock HAL binds the kernel's `hci0`. Keep
`vendor.ser.bt-uart` unset so the HAL follows the kernel HCI path.

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

The stock HAL runs in AOSP's `hal_bluetooth_default` domain. The shared fixed
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
   shows the stock service and inspect init/service logs for its
   `UNABLE_TO_OPEN_INTERFACE` result when no controller exists.

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

Dynamic selection, persisted adapter preference, hotplug switching, and the
empty VHCI emulator are intentionally removed. The stock HAL supports the
kernel's default controller interface; a later need for selection would
require a small HAL fork/proxy, but no such fork is included now.
