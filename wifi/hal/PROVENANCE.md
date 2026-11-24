# Wi-Fi HAL provenance

This directory is based on AOSP's AIDL default Wi-Fi HAL from
`hardware/interfaces/wifi/aidl/default/` at
`0162af698935100a590b7359581ac8b1b80693e5` (the checked-out
`hardware/interfaces` HEAD). The copied AOSP implementation is Apache-2.0;
its original copyright and license headers are retained. The only new source
files are the nl80211 combination reader and this provenance record.

MatonOS changes:

- Renamed the Soong defaults, library and service modules so they coexist with
  AOSP's default implementation. The binary is
  `android.hardware.wifi-service.matonos`; its init rc and VINTF declaration
  are supplied by the radio APEX rather than installed from `/vendor`.
- Removed the copied upstream unit-test module and lazy service module. The
  APEX service is started asynchronously from the APEX rc after
  `apex.all.ready`; it is disabled by default and has no `critical`, `exec`,
  or property wait action.
- Added `wifi_runtime_combinations.cpp`. After the legacy backend starts, the
  service queries `NL80211_CMD_GET_WIPHY` for the current `wlan0` ifindex and
  translates `NL80211_ATTR_INTERFACE_COMBINATIONS` into AIDL chip modes. STA,
  AP, P2P and NAN are advertised only when reported by the phy. Missing
  `wlan0`, a query failure, or an empty/unknown combination list leaves the
  chip with no advertised modes; compile-time defaults are not substituted.
  The receive socket has a one-second timeout.
- The generic legacy backend is AOSP's goldfish `libwifi-hal-emu`, from
  `device/generic/goldfish/wifi/wifi_hal/` at goldfish commit
  `296e55aa0244e8929e393e00e34471fef2a5d662`. Its source headers and the
  `device_generic_goldfish_license` module identify Apache-2.0. It uses
  nl80211/rtnetlink and has no goldfish-only driver command protocol. The
  device's Wi-Fi Soong config selects this backend for `libwifi-hal`; the
  MatonOS AIDL HAL adds phy-specific concurrency discovery directly because
  the goldfish legacy backend leaves the legacy concurrency callback
  unsupported.

No AOSP source was edited. No extra SELinux allow rule was added; the APEX
exec path uses the stock `hal_wifi_default_exec` type and transitions to
`hal_wifi_default`.
