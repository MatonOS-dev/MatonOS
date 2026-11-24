# Forked AIDL Wi-Fi HAL

## Design

`android.hardware.wifi-service.matonos` is the AOSP default AIDL Wi-Fi HAL
fork. The radio APEX carries its executable, init rc, and `IWifi/default`
VINTF declaration. The binary uses AOSP's generic nl80211 goldfish legacy
backend; the APEX starts the non-lazy service asynchronously after activation.
The service is optional to boot. If the radio APEX is absent, its rc and VINTF
declaration are absent too, and based on AOSP continues without a vendor Wi-Fi
HAL.

The AIDL service reads the selected interface's `NL80211_ATTR_INTERFACE_COMBINATIONS`
from the `wlan0` phy after the legacy backend starts. It reports only the STA,
AP, P2P and NAN modes listed by that phy. If `wlan0` is absent, the query
times out, or the driver does not report combinations, the chip advertises no
modes; it does not substitute a compile-time matrix. AP creation uses the
stock default allocator: with STA+AP concurrency it chooses `wlan1`. wifid
parks other physical interfaces under `wlan_int*`, so they do not take that
name.

The legacy backend discovers `wlan0` and `wlan1` through nl80211/rtnetlink.
It does not load modules. ODM owns module loading and the hwsim fallback. With
no card, backend initialization can still complete with no interfaces; the
AIDL service stays registered, but requests to create an interface return an
unavailable/error status. On an interface rename while Wi-Fi is disabled,
there are no active framework interface requests; the next framework start
queries the current `wlan0`. On unplug, kernel interface operations fail and
are returned to the framework; the HAL has no retry loop. Re-enabling Wi-Fi
restarts the chip and re-queries the selected phy.

## Framework and SELinux configuration

WifiService selects a declared HIDL or AIDL Wi-Fi HAL through the stock HAL
manager; this checkout has no no-vendor-HAL resource switch to set. The
service's VINTF fragment is inside the APEX, so an absent APEX does not leave
a stale declaration. The Wi-Fi feature XML remains; Wi-Fi Direct's feature
XML was removed because generic PC radios do not all expose P2P. No SoftAP
feature XML is added. SoftAP availability is governed by the runtime chip
mode and the interface combinations reported by the radio.

The service uses the existing `hal_wifi_default` domain and
`hal_wifi_default_exec` type in the APEX file-contexts. It needs `NET_ADMIN`
and `NET_RAW`; no SELinux allow rule or policy file was added.

## Build and test notes

The coordinator must build `android.hardware.wifi-service.matonos` in the
radio APEX bootstrap pass, assemble the APEX, and select the prebuilt in the
final pass. This worktree was not built or booted under the task constraints.
The checked-in coordinator image must still confirm Soong compilation, APEX
library closure, VINTF aggregation, SELinux labels, runtime binder startup,
and actual nl80211 response parsing on hwsim and physical drivers.

The AOSP test helper at
`device/generic/goldfish/wifi/mac80211_create_radios` is not in the MatonOS
product package list; the goldfish generic product includes it. The
coordinator can build that target and run it as root on a test image with
`mac80211_create_radios 2 5555` to create two hwsim radios. The MatonOS hwsim
module is also configured by `persist.vendor.maton.wifi.hwsim_radios` before
its first load, but that setting alone does not add radios after the module
has already loaded.

After a fresh coordinator build and boot:

1. With no physical radio, confirm `sys.boot_completed=1`, inspect
   `service check android.hardware.wifi.IWifi/default`, and run `cmd wifi
   status`. If hwsim is loaded, check that `wlan0` exists and that Wi-Fi scans
   complete with no networks. If hwsim is unavailable, confirm Wi-Fi requests
   return errors without delaying boot or restarting the HAL in a loop.
2. With one radio, inspect `iw list` and the HAL's `getUsableChannels` and
   mode results; verify STA and AP modes are limited to the radio's nl80211
   combination data.
3. With two radios from `mac80211_create_radios`, verify `wifid` selects one
   as `wlan0` and renames the other to `wlan_int*`. Disable Wi-Fi, switch the
   selected radio, re-enable, and confirm the new phy's combinations are used
   and AP allocation does not collide with a parked interface.
4. Unplug the selected USB adapter while Wi-Fi is enabled. Confirm HAL calls
   fail cleanly, the framework reports disconnect/unavailable, and neither
   the service nor boot enters a retry/crash loop. Reconnect and re-enable.
5. On the Ryzen 5800X/RX 6600 PC with Intel 7265, test WPA2/WPA3 scan,
   association, DHCP, and DNS; on the Surface Pro 3 with Marvell 88W8897,
   repeat scan/connect and suspend/resume. On the HP ProDesk 600 G1, test the
   no-hardware path.

### Unverified

- No Soong build, APEX assembly, image build, QEMU boot, or physical adapter
  test was run.
- The AOSP Wi-Fi HAL and goldfish legacy backend have not been compiled
  together in this worktree. The runtime parser needs coordinator build
  validation against the generated AIDL headers and the target libnl version.
- NL80211 GET_WIPHY behavior with IFINDEX needs validation on mac80211_hwsim,
  Intel 7265, and Marvell 88W8897.
- Service lookup when the APEX is absent, plus APEX linker namespace and
  VINTF aggregation, need confirmation in a fresh image.
- No kernel change is requested. No new SEPolicy allow is requested.
