# wifihal1 report

## Commit

Implementation commit: `f975e80a2112323eb7897ad646a4ec08df3cb754`
(`wifi: add AIDL HAL radio APEX fork`). This report is a follow-up commit.

## Files and behavior

Forked `hardware/interfaces/wifi/aidl/default` at
`0162af698935100a590b7359581ac8b1b80693e5` into `wifi/hal/`. The fork is
Apache-2.0 and its source headers are retained. `wifi/hal/PROVENANCE.md` lists
the source revisions and fork changes.

The APEX binary is `android.hardware.wifi-service.matonos`; `radio/apex/`
now includes it in `payload.list`, declares `IWifi/default` in its VINTF
manifest, labels it with stock `hal_wifi_default_exec`, and starts it from the
`apex.all.ready=true` action. The service is `disabled` and started
asynchronously with `NET_ADMIN` and `NET_RAW`; there is no `critical`, `exec`,
or `wait_for_prop`. When the APEX is absent its VINTF fragment and rc are
absent too, so WifiService does not see a declared service to wait for. With
the APEX active, AOSP activates APEXes before starting the framework; runtime
registration and that ordering still need image confirmation.

## Legacy backend and combinations

Selected AOSP's goldfish generic nl80211 backend (`libwifi-hal-emu`) from
`device/generic/goldfish` commit
`296e55aa0244e8929e393e00e34471fef2a5d662`. Its nl80211/rtnetlink behavior
fits generic cfg80211 cards. The alternative `frameworks/opt/net/wifi/libwifi_hal`
fallback installs stubbed operations and has no usable generic radio backend.
The emulator backend does not implement the legacy concurrency-matrix call,
so the fork queries the kernel directly.

After the backend starts, `wifi_runtime_combinations.cpp` sends
`NL80211_CMD_GET_WIPHY` for `if_nametoindex("wlan0")`, parses
`NL80211_ATTR_INTERFACE_COMBINATIONS` and `NL80211_ATTR_SUPPORTED_IFTYPES`,
and maps the kernel's limits to AIDL STA/AP/P2P/NAN modes. nl80211's implicit
single-interface modes are added from supported interface types. The query
socket has a one-second receive timeout. No compile-time
`WIFI_HAL_INTERFACE_COMBINATIONS` setting is provided by this fork. If the
interface is missing or the query has no usable result, the HAL advertises no
modes instead of claiming unsupported STA/AP concurrency.

When the radio reports STA+AP support, stock `WifiChip` allocates the AP
interface as `wlan1`. wifid renames other selected-area interfaces to
`wlan_int*`, so its names do not occupy `wlan1`.

## Failure behavior

- **No card:** the goldfish backend can initialize with an empty interface
  list. The AIDL service remains registered; no `wlan0` means no advertised
  mode, and interface requests return normal unavailable/error statuses. If
  wifid's hwsim fallback exists, the query instead uses that `wlan0`.
- **wifid rename while Wi-Fi is disabled:** there are no active framework
  interface requests during the rename. On the next Wi-Fi HAL start the
  runtime query resolves the new `wlan0` ifindex. The bridge's disable-before-
  rename behavior is assumed; this lifecycle needs fresh-image verification.
- **Unplug:** operations against the removed ifname fail through the existing
  legacy HAL and are returned to the framework. This code adds no retry loop
  or blocking wait. Re-enabling Wi-Fi restarts the chip and re-queries the
  selected phy.

## Framework configuration and features

`WifiHal` selects an AIDL HAL from its VINTF declaration; this checkout has no
separate no-vendor-HAL resource flag to set. `wifi/wifi.mk` adds the forked
binary only in the bootstrap pass and removes the unconditional Wi-Fi Direct
feature declaration: generic cards do not all support P2P. The general Wi-Fi
feature XML remains. There is no SoftAP feature XML among the AOSP Wi-Fi
feature declarations, so none was added. STA/AP availability comes from the
runtime matrix. No framework or other AOSP source was patched.

## SELinux and kernel

The executable uses `/bin/hw/android.hardware.wifi-service.matonos` in the
radio APEX file contexts, labeled `hal_wifi_default_exec`; stock policy
transitions it to `hal_wifi_default`. No new allow rule, policy file, or
kernel configuration change is included.

## Two-radio test hook

`device/generic/goldfish/wifi/mac80211_create_radios` is a goldfish product
package, but is **not** in MatonOS `PRODUCT_PACKAGES`. The coordinator can
build the AOSP module target `mac80211_create_radios`, copy the generated
`vendor/bin/mac80211_create_radios` to `/data/local/tmp` in the disposable
test image, and run as root:

```sh
/data/local/tmp/mac80211_create_radios 2 5555
```

This creates two hwsim radios at runtime. Alternatively, before the first
hwsim module load, set `persist.vendor.maton.wifi.hwsim_radios=2`; it does not
add radios to a module that is already loaded.

## Verification and remaining unknowns

Static checks passed: `git diff --check`, XML parsing of both VINTF
manifests, and clean commit status after the implementation commit. No Soong
build, radio APEX assembly, image build, VM, or hardware test was run, as
directed. Therefore the feature is not yet compile- or runtime-verified.

The coordinator still needs to verify:

- the fork and helper compile against the checkout's generated AIDL headers
  and libnl;
- `GET_WIPHY` with IFINDEX returns the expected attrs on hwsim, Intel 7265,
  Marvell 88W8897, and generic cfg80211 devices;
- the goldfish legacy backend handles absent interfaces, wifid renames, and
  unplug without stale handles or crashes;
- APEX vendor-library closure, service labeling, VINTF aggregation, and
  framework service startup in a fresh image;
- STA scan/connect, AP creation only for capable phys, two-radio switching,
  unplug/reconnect, and no-hardware boot completion.
