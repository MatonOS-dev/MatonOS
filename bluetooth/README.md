# Bluetooth

MatonOS packages AOSP's stock `android.hardware.bluetooth-service.default`
Linux HCI AIDL HAL. The HAL uses the kernel's Bluetooth management channel to
unblock rfkill and discover the controller, then opens the standard HCI user
channel. It registers the stable `IBluetoothHci/default` instance for the
Android Bluetooth stack.

With no controller, the stock HAL reports initialization failure and the
Bluetooth stack should remain off/unavailable. No virtual controller is
created. Controller selection, hotplug replacement, and the MatonOS Bluetooth
adapter picker are dropped; the stock HAL binds the kernel's `hci0`. Keep
`vendor.ser.bt-uart` unset so the HAL follows the kernel HCI path.

Bluetooth audio is deferred to v5. The ODM audio policy has no Bluetooth audio
module, and no live audio code reads `vendor.maton.bluetooth.present`.

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
`IBluetoothHci/default` initializes without an HCI bind error. QEMU's generic
environment does not pass through host controllers, so the real-controller
case needs physical hardware.

## Real hardware test sequence

1. On the Ryzen 5800X PC with Intel 7265 Wi-Fi+Bluetooth, confirm the stock
   HAL initializes `hci0`, then scan, pair a keyboard or mouse, verify HID,
   and transfer a small file.
2. On Surface Pro 3, confirm the Marvell 88W8897 controller and firmware
   initialize `hci0`, then scan and pair a device.
3. On HP ProDesk 600 G1 with no Bluetooth controller, confirm boot completes,
   Bluetooth stays unavailable/off, Developer options opens, and no Bluetooth
   process abort loop occurs.

## Dropped behavior

Dynamic selection, persisted adapter preference, hotplug switching, and the
empty VHCI emulator are intentionally removed. The stock HAL supports the
kernel's default controller interface; a later need for selection would
require a small HAL fork/proxy, but no such fork is included now.
