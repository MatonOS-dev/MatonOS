# Wi-Fi

## Design

MatonOS uses the AOSP Wi-Fi framework and Settings UI with Linux `nl80211`:
`wpa_supplicant` AIDL, `wificond`, and Android's network stack. There is no
vendor `IWifi` HAL. The stock stack owns the kernel interfaces; this keeps the
normal PC driver and firmware path intact. `wifi.interface=wlan0` remains the
framework's interface setting. `wifi/wifi.mk` stages the supplicant template,
the Wi-Fi feature declaration, and the signed `regulatory.db` files.

`matonos-wifid` is retained only for Settings' existing `wifi` control channel
and for a single early hardware snapshot. It does not rename interfaces,
block radios, subscribe to hotplug, or poll sysfs. At its one-shot startup it
looks for physical `phy80211` devices. If none exist, it asks init once to
load `mac80211_hwsim` with its default one-radio parameter. This gives generic
PCs a normal empty nl80211 `wlan0`; the fallback is never requested when a
physical phy was present in the snapshot. A module-load failure leaves Wi-Fi
unavailable and does not delay boot. Real adapter/interface choice remains
the stock kernel/Android stack's responsibility.

The daemon publishes `vendor.maton.wifi.present` and retains
`persist.vendor.maton.wifi.device` as the Settings selection preference.
`get_state`, `list_devices`, and `select_device` remain available on
`/dev/socket/matonos/wifi`. Selection records a discovered physical device
identifier; it does not rename or power-manage hardware. The process stays
resident only because the shared IPC channel is hosted by the daemon. Its
startup work is bounded and independent of whether any radio exists.

The supplicant copies `/vendor/etc/wifi/wpa_supplicant.conf` to vendor data
when WifiService creates its STA interface. Init prepares its data directories
before Settings requests Wi-Fi. The generic `CONFIG_DRIVER_NL80211` backend
supports generic PC chipsets. `regulatory.db`, its signature, and the upstream
license are staged from the pinned wireless-regdb release.

## Build and verification

The coordinator owns AOSP builds. The native code is built with
`MATON_BUILD_COORDINATOR=1 bash tools/build-native.sh -j4`; the repository
preflight is `MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh`. A fresh image
must pass `tools/check-selinux-labels.sh` before runtime testing.

After a fresh boot, check:

```sh
getprop vendor.maton.wifi.present
getprop persist.vendor.maton.wifi.device
ip link show wlan0
cmd wifi status
```

With no Wi-Fi device, verify boot reaches `sys.boot_completed=1`, the empty
hwsim radio provides `wlan0` where the kernel module loads, Wi-Fi scans return
no networks, and Developer options opens. If the module is unavailable, Wi-Fi
should remain cleanly unavailable without affecting boot. With a physical
adapter, verify the kernel's real interface is used and no hwsim radio appears
beside it. Use Settings to enumerate and select a device, then verify the
preference persists across reboot. On real hardware test scan, WPA2/WPA3
connection, DHCP, DNS, Ethernet coexistence, and suspend/resume.

### Hardware checklist

1. Ryzen/RX 6600 PC with Intel 7265: scan/connect, WPA2/WPA3, DHCP/DNS,
   Ethernet coexistence, and adb over Ethernet.
2. Surface Pro 3 with Marvell 88W8897: cold-boot scan/connect/DHCP and
   suspend/resume; if power-save problems occur, compare `disable_auto_ds=1`
   as documented in the kernel module notes.
3. HP ProDesk 600 G1 without Wi-Fi: boot, confirm hwsim or clean unavailability,
   open Developer options, and then repeat with a supported USB adapter.

## Open issues

- This change still needs a fresh-image QEMU boot and physical adapter tests.
- The selection API stores a preference, while the stock stack chooses the
  active kernel interface; honoring multiple-adapter selection without
  renaming, rfkill, or a custom HAL needs a stock-stack mechanism.
- Hotplug is left to the kernel and stock Wi-Fi stack; the daemon does not
  refresh its Settings device list after startup.
- The WPA2 hwsim AP fixture is no longer part of this daemon.
- Update the regulatory database when the pinned kernel/regdb version changes.
