# Morning handover 2026-11-25 (overnight 11-24 → 25)

Branch `wip/audio-apex` (dt-audioapex). Status as of the last watchdog run;
newest entries at the bottom of each section.

## Builds
| build | dt | result |
|---|---|---|
| 126 | a89075a0 | OK 22:43; live126 tested (below) |
| 127 | f106f7ac | FAILED Soong 22:53: BT fork linked `libbluetooth_offload_hal`, not visible to device trees |
| 128 | 3fa4ed6a | FAILED 23:19 compile: `try` in BluetoothHci.cpp (vendor = -fno-exceptions; my syntax check lacked the flag) |
| 129 | 2a6bd333 | running since 23:21: BT fix (no try/catch, -Werror initializers) + first Wi-Fi HAL compile |

## Done overnight
- **live126** (enforcing, boot ~40 s): zram swap 8 GB OK. swappiness stayed 60
  (vendor_init denied on plain `proc`) → fixed for 128 with a vendor label
  `proc_maton_swappiness` (2bab41ac).
- **wifid** had no hotplug on 126: netlink_route_socket denied in
  `matonos_driver`. Renaming also needs net_admin + udp ioctls, which
  hal_neverallows forbids for that HAL-server domain → wifid now has its own
  domain `matonos_wifid` (2bab41ac). In permissive the monitor worked and only
  needed what the new domain grants.
- **wifid bug**: logged "publish failed" on every successful delivery
  (publish returns listener count) → 950096d2.
- **Two-radio test** could not run on 126: hwsim genl NEW_RADIO, rmmod and
  even klogctl return EPERM as root with full caps in permissive, no AVC.
  Unexplained (capable() fails; bpf LSM suspicion, not chased). Workaround:
  `persist.vendor.maton.wifi.hwsim_radios` sets the boot-time count
  (b312b502). Alternative from codex: build `mac80211_create_radios`.
- **BT HAL fork without the Rust offload proxy** (3fa4ed6a): local
  `hci_interface.h` keeps the ffi C++ interface, `aidl_service.cpp` is a plain
  NDK BnBluetoothHci (death → clientDied). Syntax-checked with AOSP clang on
  the server against the generated NDK headers (caught 3 bugs). Lost: HCI
  A2DP/LE offload modules (unused; LE Audio is software in the media APEX).
- **Wi-Fi HAL fork** (codex wifihal1, merged 1cab10fa): stock AIDL default
  HAL → `wifi/hal/`, goldfish nl80211 legacy backend (`BOARD_WLAN_DEVICE :=
  emulator`, already set), interface combinations read from the wlan0 phy via
  nl80211 GET_WIPHY at runtime, AP iface `wlan1`, started non-blocking from the
  radio APEX rc. My fixes: licence visibility (would have failed like 127),
  kept the Wi-Fi Direct feature xml codex had removed.
- **Keys**: old `com.matonos.hardware.{audio,pipewire}.*` payload keys deleted
  on server and host (no non-doc references left). Media + radio keys are
  created by the first successful build → back up then.

## Still to test (build-128, then 129)
1. Both APEXes active (`com.matonos.hardware.media`, `.radio`).
2. BT: no adapter → enable → disable during wait → plug (host 8087:0a2a
   passthrough) → enable = ON; give the adapter back to the host after.
3. Boot with the radio APEX removed completes.
4. Audio unchanged after the build-audio-apex refactor.
5. swappiness 180, wifid in `matonos_wifid` with no denials.
6. Two hwsim radios (`hwsim_radios=2`), bridge switch to the second card.
7. (129) Wi-Fi HAL: registers, STA scan/connect on hwsim, no crash loop with
   no card, survives switch/rename.

## Notes for you
- Minecraft on build-server: the build wrapper restarts it only if it was
  running when the build started; it was, so it is up again. Left alone.
