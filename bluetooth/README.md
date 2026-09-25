# Bluetooth

MatonOS keeps the AOSP Bluetooth stack, Settings UI and APIs. `matonos-btd`
owns controller discovery and selection in the shared `matonos_driver`
SELinux domain. It keeps an active real controller while it exists, honors `persist.vendor.maton.bluetooth.device`, and otherwise
prefers built-in PCI/SDIO/UART devices over USB at boot. It soft-blocks unused
controllers through rfkill; the HCI HAL unblocks the selected controller on
initialize. USB hotplug is observed through kernel uevents. When a selected
controller disappears, the manager chooses another real device or creates the
virtual fallback.

The custom NDK AIDL service implements `IBluetoothHci/default` over Linux's
HCI user channel. It opens the `hciN` index published by the manager, so the
stack sees the selected real or virtual controller. `vendor.maton.bluetooth.present`
is `1` only when a real controller is selected and is published before
audioserver starts; the audio HAL uses it to gate Bluetooth audio modules.
`vendor.maton.bluetooth.available` is set once either backend is ready.
Bluetooth and BLE package features remain declared on all PCs.

With no physical controller, the manager opens `/dev/vhci` and starts its own
small HCI command emulator. The kernel `hci_vhci` module creates a normal HCI
device; the emulator answers controller setup and scan commands without
emitting advertising reports. This keeps Bluetooth Settings, the toggle, and
empty BLE scans functional without shipping Rootcanal's large host simulator
and private build dependencies. This fallback is intentionally an empty-radio
spoof: it cannot pair to another Rootcanal process, emulate HID, or transfer
files. A real controller provides those functions. Pairing and data profiles
depend on the actual controller and AOSP stack.

After the early vendor mount, init loads the already-built `rfkill.ko`,
`bluetooth.ko`, and `hci_vhci.ko` modules; no kernel build or host Bluetooth
passthrough is required. Firmware loading remains in the kernel. The manager checks
`/vendor/firmware/intel`,
`rtl_bt`, and `mrvl` and logs when firmware is missing. Staged firmware
includes Intel `ibt-*`, Realtek `rtl_bt/*`, and Marvell `mrvl/*`; exact
compatibility depends on the controller revision.

## Device selection interface

The persisted selector accepts a sysfs parent basename (for example a USB or
PCI ID) or an `hciN` name. The manager publishes the selected ID and index in
vendor properties. The bridge's generic JSON socket is
`/dev/socket/matonos/bluetooth`:

- `list_devices` returns `{"devices":[{"id":"...","hci":"hci0","index":0,"usb":false}]}`.
- `select_device` accepts `{"id":"<listed stable ID>"}` and persists that
  choice for future hotplug or boot selection.

Only currently enumerated adapters can be selected through the socket. A
privileged setup can set the persisted property directly before an adapter is
plugged in.

## Build and verify

Build the NDK daemons with `bash device/maton/pc_x86_64/tools/build-native.sh
-j2`. The Bluetooth service uses AOSP-generated NDK AIDL sources with
`--structured --stability=vintf`, and links the platform `libbinder_ndk` so
it can register its VINTF-stable HAL service. The area binaries are imported
through fixed buildinfra prebuilts. For the image build and SELinux label
check, request the build from the coordinating agent; area agents do not run
AOSP builds.

The manager and HAL both compiled successfully as Android 35 x86_64 NDK PIE
executables with Clang 21/CMake, using two workers. AIDL generation succeeded
with the VINTF structured backend flags. This validates compilation only; the
fresh-image QEMU checks below are pending the coordinator's shared build.

On a fresh QEMU boot without host passthrough, check that
`vendor.maton.bluetooth.present=0`, `vendor.maton.bluetooth.virtual=1`, the
VHCI index is nonnegative, `matonos-bluetooth` is running and the Bluetooth
Settings toggle works. Run a BLE scan; it should complete empty with no
`com.android.bluetooth`/HAL crash loop. A second Rootcanal peer cannot pair
with this minimal empty-radio fallback.

## Real hardware test sequence

1. On the Ryzen 5800X/RX 6600 PC with Intel 7265 Wi-Fi+BT, verify
   `present=1`, `virtual=0`, a real `hciN` selection and no missing firmware
   errors. Turn Bluetooth on, scan, pair a keyboard or mouse, verify HID
   input, and transfer a small file.
2. Select a USB Bluetooth dongle by its stable ID, unplug it, and confirm the
   manager chooses the remaining controller or VHCI fallback. Reconnect it
   and confirm the configured preference wins.
3. On Surface Pro 3, check Marvell 88W8897 firmware, scan, pairing, HID and
   file transfer. Bluetooth audio profiles depend on the audio agent's
   conditional modules.
4. On HP ProDesk 600 G1 without a Bluetooth controller, verify the empty
   VHCI fallback and Settings toggle. Then hotplug a USB dongle and verify
   transition to its real HCI device; unplug it and verify fallback again.

## Open issues

- The virtual HCI emulator implements the controller initialization and
  command-complete path needed for an empty scan, not a complete Bluetooth
  controller. Its compatibility must be checked on the fresh QEMU image.
- The manager blocks unused controllers. The HCI HAL soft-unblocks the selected
  real controller on initialize and blocks it on close, following Android's
  radio lifecycle.
- The daemon list/select socket API is present. No MatonOS Settings page or
  per-app System Bridge target allowlist entry consumes it yet.
- UART device discovery depends on the machine's existing serdev setup and
  firmware configuration.
