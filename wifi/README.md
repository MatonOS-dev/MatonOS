# Wi-Fi

## Design

MatonOS uses the AOSP Wi-Fi framework and Settings UI with Linux `nl80211`.
`wpa_supplicant` provides the supplicant AIDL service, `wificond` handles
scanning and interface control, and Android's network stack provides DHCP.
`matonos-wifid` owns adapter discovery and selection beneath those services.
The daemon is built with the NDK from `native/matonos-wifid/`, linked to the
shared IPC helper, and installed by the ODM bundle at `/odm/bin/matonos-wifid`.
Its executable, init service, and Wi-Fi properties are registered in
`bundle/contents.list`. It presents the selected adapter as `wlan0`; Ethernet
remains under `netd` and the platform network stack.

No vendor `IWifi` HAL is shipped. On this AOSP branch, `WifiNative` supports
devices without one: it uses `wifi.interface=wlan0`, bypasses
`HalDeviceManager`, and performs scan/interface work through its nl80211 path.
The WifiService resource enables wificond migration. The supplicant AIDL
service and its VINTF fragment are installed by `wpa_supplicant`. The generic
`CONFIG_DRIVER_NL80211` backend is enabled for generic PC chipsets.

The `android.hardware.wifi` feature is always declared. When no physical radio
exists, the daemon asks init to load `mac80211_hwsim` with `radios=1`; this
provides an ordinary nl80211 radio with no access point. Android's normal
Wi-Fi service, scans, toggles, and Settings UI therefore remain active and
show no available networks. `virt_wifi`, which displays Ethernet as a
connected Wi-Fi AP, is reserved for QEMU-specific tests.

The stock AIDL supplicant lazily copies `/vendor/etc/wifi/wpa_supplicant.conf`
to `/data/vendor/wifi/wpa/wpa_supplicant.conf` when WifiService creates its
STA interface. The vendor image supplies that template, and init creates the
vendor-data directories on `apex.all.ready` before the supplicant is requested.
Without the template, `addStaInterface(wlan0)` fails and WifiService drops back
to DisabledState even though hwsim and wificond are running.

### Adapter policy and controls

At boot the daemon allows ten seconds for driver coldplug. It honors
`persist.vendor.maton.wifi.device` if it matches a real adapter's MAC address,
phy name, interface name, or canonical sysfs device path. Otherwise it prefers
built-in PCIe/SDIO devices over USB. The active real adapter remains selected
while it exists; a newly added USB adapter does not preempt it. If the active
adapter disappears, the daemon chooses the persisted adapter if available,
then the built-in-first default. When no real adapter remains, hwsim becomes
`wlan0`. A real adapter appearing later replaces hwsim; if it is removed, the
daemon restores hwsim as `wlan0`.

The daemon discovers radios from their `phy80211` sysfs links and identifies
hwsim by its sysfs device path. Other Wi-Fi netdevs are brought down; unused
`wlan*` names are changed to private `mtnw*` names. It reads
`/sys/class/rfkill` and writes `/dev/rfkill`: unused real adapters are
soft-blocked, while the selected adapter stays unblocked so Android can bring
`wlan0` up. Blocking the selected radio while the interface was down prevented
Wi-Fi enable from bringing it up. Physical rfkill remains controlled by the
hardware switch or firmware. A driver-created
nl80211 phy/netdev indicates that firmware initialization completed.

The daemon publishes `vendor.maton.wifi.present=0|1` and
`vendor.maton.wifi.selected=<MAC>` (`hwsim:<MAC>` for the spoofed radio). The
persisted property is the administrative real-device selection interface.
The daemon runs in the shared `matonos_driver` SELinux domain with
interface-configuration ioctls, sysfs discovery access, rfkill access, and
status-property access from the fixed policy files in `sepolicy/matonos/`.
It serves the generic bridge protocol on
`/dev/socket/matonos/wifi`: `get_state` returns presence/selection and test AP
readiness, `list_devices` returns the discovered adapters and their built-in,
simulated, and selected flags, and `select_device` accepts
`{"device":"<MAC, interface, phy, or sysfs path>"}` for a discovered real
adapter. Its `state` topic reports presence/selection changes. Only System
Bridge is allowed to connect; the `wifi org.matonos.settings` allowlist row
and typed Settings wrapper are in `systembridge/` and `buildinfra/client/`.

`regulatory.db` and its signed `regulatory.db.p7s` come from the official
wireless-regdb 2026.09.03 release and are copied to `/vendor/firmware`; the
upstream license is included. The Wi-Fi feature declaration and regulatory
files stay in vendor because framework and kernel consumers resolve them
there. The executable, init rc, and defaults are supplied by the ODM bundle.
The staged mainline kernel configuration requires signed regulatory data and
includes the wireless-regdb trusted key.

## Surface Pro 3 / mwifiex

The Marvell 88W8897 uses `mwifiex_pcie`. If it has missed wakeups, slow scans,
or unreliable reconnects, test `disable_auto_ds=1` to disable firmware
automatic deep sleep; this can increase battery use. From a root shell before
the driver is loaded:

```sh
rmmod mwifiex_pcie mwifiex 2>/dev/null
insmod /vendor/lib/modules/mwifiex.ko
insmod /vendor/lib/modules/mwifiex_pcie.ko disable_auto_ds=1
```

Supply module parameters when loading. Do not unload the module while
connected. Compare suspend/resume, reconnect, and battery drain before making
this a default. No default override is applied because power use varies by
hardware revision.

## Build and verification

The coordinator owns AOSP builds. The Wi-Fi full-image request is recorded in
`~/Documents/aosp/out/pc-logs/agents/build-requests.txt`. The coordinator
reports the image path and build result in
`~/Documents/aosp/out/pc-logs/agents/build-status.txt`; `tools/check-selinux-labels.sh`
must report no FAIL for that image. The native daemon can be rebuilt outside
Soong with `bash device/maton/pc_x86_64/tools/build-native.sh -j4`.

After a fresh boot, check `getprop vendor.maton.wifi.present`,
`getprop vendor.maton.wifi.selected`, `ip link show wlan0`, and `cmd wifi
status`. In Settings, confirm the Internet panel shows Wi-Fi and Ethernet
separately. With a real AP, scan and connect with WPA2-Personal and
WPA3-Personal; verify DHCP, DNS, internet access, and Ethernet coexistence.
For a no-radio test, run `svc wifi enable`, confirm `cmd wifi status` reports
STA enabled, then run `cmd wifi start-scan` and `cmd wifi list-scan-results`.
The empty hwsim airspace should return an empty scan list without disabling
Wi-Fi.

### QEMU tests

1. **No adapter:** boot without Wi-Fi hardware. Confirm the daemon loads hwsim,
   `wlan0` exists, `cmd wifi` can enable Wi-Fi and scan, the scan list is empty,
   Settings opens without crashes, and Ethernet/adb still work.
2. **WPA2 access point:** before rebooting, set
   `persist.vendor.maton.wifi.hwsim_radios=2`. The daemon requests two hwsim
   radios for this test instead of its normal one-radio fallback, keeps the
   Android client on `wlan0`, and reserves the second radio as `wlan1` for a
   guest hostapd test process (`vendor.maton.wifi.hwsim.test_ready=1`). Start
   a WPA2-Personal AP on `wlan1`, scan from Settings or `cmd wifi`, connect to
   that SSID on `wlan0`, and verify DHCP and internet connectivity. Keep adb
   on a separate virtio Ethernet adapter. The standalone hostapd test process
   is not shipped in the image; a guest test harness must provide it.
3. **virt_wifi:** only for the cuttlefish-style synthetic connected-AP test,
   load `virt_wifi` over the guest virtio Ethernet interface. Verify the
   framework sees the synthetic AP. Do not use this path for WPA2 testing.

These tests exercise the Android Wi-Fi stack and do not validate physical
firmware or WPA3 interoperability.

### Real hardware checklist

1. On the Ryzen/RX 6600 PC with Intel 7265 Wi-Fi, verify scans, WPA2/WPA3
   connection, DHCP, DNS, internet access, Ethernet coexistence, and adb over
   Ethernet.
2. On the Surface Pro 3 with Marvell 88W8897 and no Ethernet, verify cold-boot
   scan/connect/DHCP and suspend/resume. If power-save problems occur, compare
   the `disable_auto_ds=1` test above.
3. On the HP ProDesk 600 G1, verify boot and Ethernet with no Wi-Fi adapter;
   then test with a supported USB adapter present at boot.
4. If available, verify WPA3-Personal SAE. Support depends on the AP, chipset,
   and firmware.

## Open issues

- Physical Wi-Fi hardware and the hwsim QEMU paths have not yet been tested
  against a fresh image after adding the vendor supplicant template.
- Adapter discovery polls sysfs every two seconds instead of subscribing to
  kernel uevents.
- The WPA2 fixture still needs a standalone guest hostapd harness; the image
  reserves and reports `wlan1` but does not ship that test utility.
- Hotspot/P2P and chipset-specific tuning are deferred.
- Update the regulatory database when the pinned kernel/regdb version changes.
