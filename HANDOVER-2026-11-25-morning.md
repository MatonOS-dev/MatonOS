# Morning handover 2026-11-25 (overnight 11-24 → 25)

Branch `wip/audio-apex` (dt-audioapex). Status as of the last watchdog run;
newest entries at the bottom of each section.

## Builds
| build | dt | result |
|---|---|---|
| 126 | a89075a0 | OK 22:43; live126 tested (below) |
| 127 | f106f7ac | FAILED Soong 22:53: BT fork linked `libbluetooth_offload_hal`, not visible to device trees |
| 128 | 3fa4ed6a | FAILED 23:19 compile: `try` in BluetoothHci.cpp (vendor = -fno-exceptions; my syntax check lacked the flag) |
| 129 | 2a6bd333 | FAILED 23:56 radio APEX packaging: "bootstrap hostapd missing". Soong compile passed (BT + Wi-Fi HAL forks built, media APEX built) |
| 130 | fb37d44d | OK 01:14 (`BOARD_HOSTAPD_DRIVER := NL80211`); live130 tested (below) |
| 131 | 25035145 | OK 02:21 (radio rc in `etc/`, BT HAL `interface` line); live131 tested (below) |
| 132 | a9fdf8a5 | OK 03:30; live132 tested (below) |
| 133 | d0d8b8b8 | OK 04:38; live133 tested (below) |
| 134 | 97019aa5 | stopped by me after ~3 min to fold in the nl80211 fix |
| 135 | ac0ec3be | OK 05:57; live135 tested (below) |
| 136 | 656a541d | OK 07:09; live136: hwsim loads at boot again (phy0 + wlan0), enforcing, 0 denials |
| 137 | 12794e03 | OK 08:34; live137 tested (below) |
| 138 | 6c3620ac | OK 09:46; live138 Wi-Fi PASS (below) |
| 139 | 7c9af06e | FAILED link 10:15: extern "C" names ignored the namespace |
| 140 | 62340005 | FAILED link 10:42: backend static lib never reached the binary link |
| 141 | 3c8f117a | OK 11:53; live141 PASS except WPA2 hotspot (below) |
| kernel | 42440352 | CONFIG_CRYPTO_CCM=y: ACK 7.3 rebuilt 12:15, staged into server prebuilt/ (old kernel in backup/prebuilt-kernel-pre-ccm) |
| 142 | 2a9c0336 | FAILED Soong: mediahal1 BT props in PRODUCT_SYSTEM_PROPERTIES (generic_system forbids) |
| 143 | 8f6fb45a | OK 13:34; live143 boots 40 s enforcing, kernel 7.3.0-g180e0b21ff6a: **WPA2 hotspot PASS** (AP-ENABLED, ccm(aes) built in), BT audio provider/default registered, LE Audio + offload=false props set (vendor), 0 crashes, 0 denials |
| 144 | f73b15ce | FAILED PipeWire RAOP: `-include` abs path in openssl.pc got sysroot-prefixed by pkg-config |
| 145 | 6e999e96 | FAILED same: meson reused cached dep flags → `meson setup --reconfigure --clearcache` |
| 146 | 03a79281 | OK 14:49; live146 boots 35 s enforcing; PipeWire loads module-maton-network-speakers. **Correction:** my earlier "0 denials" (143–146) used a grep that missed auditd's `avc:  denied`; real denials found: kernel can't read regulatory.db (Wi-Fi world domain only), bridge NSD `find` (speaker browse NPE every 5 s), PipeWire DMI/PCI reads → fixed in 2eb1da94 + 06cfdda2 (build-148). Still open, pre-existing: init `mounton /oem`, vendor_init `search oem` |
| 147 | 7901c583 | OK 16:00 (bionic-fill, openssl.pc bn.h, btd compile). Not VM-tested: superseded by 150 |
| 148 | ed3229c8 | FAILED 16:04 in subsystem PipeWire: cross-file c_args only apply at first meson setup, server build dir reused → no bionic-fill. Fix sub f09cf02b (c_args via -D + --clearcache; Xwayland helper rebuilds when cross file changes) |
| 149 | ed3229c8 | FAILED 16:08: force-included fills need cpu_set_t/funopen64, broke meson's plain probes (“8-byte atomics required”). Fix: bionic-fill 50e5459 gates GNU fills on _GNU_SOURCE (like glibc); pins sub 1267963f, dt 58b41da8; sub refetches bionic-fill on pin change |
| 150 | 58b41da8 | OK 17:22. live150 boots enforcing; pipewire, wireplumber, btd, BT HAL, wifid running; no crashes; NSD NPE gone. Still: regulatory.db firmware_load denied (-13), bridge bluetooth_manager find denied, btd debug log every 1 s, ~100 WirePlumber sysfs search denials (libudev-zero enumerates everything). Pre-existing: vendor_init oemfs search, vendor_modprobe key write, vendor_init proc write, pc_gpu_detect sys_admin |
| 151 | 8a80dcc0 | running since 18:54: regdb firmware_load, bridge bluetooth_manager find, WirePlumber sysfs dontaudit, btd debug line removed. Only live-150.img kept |

**live130 results** (enforcing, booted in under a minute):
- PASS: both APEXes active (`media@1795560288`, `radio@1795560296`).
- PASS: swappiness 180 (vendor label fix works).
- PASS: audio. pipewire, wireplumber, matonos-audio-hal, effect HAL and
  audioserver all run from the media APEX; the output thread wrote frames;
  no denials at all.
- FAIL → fixed for 131: no radio service existed. The rc sat in
  `etc/init/`, but init only reads `/apex/<name>/etc/*.rc`. Boot still
  completed without any of them, which is the no-block rule working.

Media + radio APEX payload keys backed up to host `~/.config/matonos/release-keys`
(hashes match the server copies, contents never printed).

**Found by 129:** hostapd has not been in any image since the hotspot commit
5dcef069. Its Soong module is only enabled when the board sets
`BOARD_HOSTAPD_DRIVER`, and a disabled `PRODUCT_PACKAGES` entry is dropped
silently. So hotspot never had a daemon; the radio APEX check is what caught it.

**live131 results** (enforcing):
- PASS: radio services run in their stock domains: Wi-Fi HAL
  (`hal_wifi_default`), BT HAL (`hal_bluetooth_default`), wifid
  (`matonos_wifid`); `IWifi/default` and `IBluetoothHci/default` registered.
- PASS: **BT sequence.** With no adapter, enable waits and a disable during
  the wait cancels cleanly. After the host Intel adapter (8087:0a2a) is
  passed through, enable reaches **ON** with the real address. The adapter
  was given back to the host afterwards (host `hci0` OK).
- FAIL → fixed for 132 (a9fdf8a5), Wi-Fi would not enable:
  1. codex bug: `SO_RCVTIMEO` was set before `genl_connect()` (EBADF), so the
     combinations query was skipped, the chip had no modes and no interface
     could be created;
  2. `hal_wifi_default` was missing a `netlink_route_socket` rule (goldfish
     carries the same one);
  3. `vendor.wlan.{firmware,driver}.version` had no label;
  4. the bridge was denied `find wifi_service` (WifiCardSwitcher).
  After 2 failed starts, WifiSelfRecovery turned Wi-Fi off, as expected.
- Fixed after 132 started (goes into 133): the BT HAL now sets an `onUnlinked`
  callback, so the "AIBinder_linkToDeath … will become an abort" warning is gone. The
  stock Wi-Fi HAL code logs the same warning; still open.

**live132 results** (enforcing, booted in ~35 s):
- PASS: zero denials. The Wi-Fi HAL read 6 combinations from wlan0's phy over nl80211.
- FAIL → fixed for 133 (d0d8b8b8), two more bugs in the forked HAL:
  1. The mode IDs changed after configureChip (legacy V1 modes swapped for one
     runtime kV3 mode), so the chip was "not configured" and no STA iface could
     be created.
  2. hwsim advertises limits of 2048 interfaces; the framework expands each
     limit as types^maxIfaces and **system_server's Wi-Fi thread crashed**
     (NegativeArraySizeException in HalDeviceManager). Limits are now capped
     at what WifiService can use (STA 2, AP 2, P2P 1, NAN 1). The framework
     crash itself is a stock robustness gap (not patched, AOSP).
- How I check HAL C++ before a build now: AOSP clang on the server against the
  generated `android.hardware.wifi-V4` + `wifi.common` NDK headers,
  `--target=x86_64-linux-android37 -nostdlibinc -fno-exceptions -Werror`.

**live133 results** (enforcing):
- **PASS: Wi-Fi turns on.** wlan0 up, wpa_supplicant running, scan
  completes (0 results, nothing in range), no denials, no crash.
- PASS: hostapd now exists and registers `IHostapd/default`.
- FAIL → fixed for 135 (ac0ec3be): hotspot. The HAL could not create the AP
  interface (`wlan2 NOT_SUPPORTED`): the goldfish legacy backend expects
  pre-made interfaces. Our HAL now creates STA/AP interfaces itself through
  nl80211 NEW_INTERFACE on wlan0's phy and deletes them again.
- FAIL → fixed for 135 (97019aa5): `persist.vendor.maton.wifi.hwsim_radios=2`
  was ignored. wifid asks for hwsim on `apex.all.ready`, before persist
  props are loaded; the rc trigger now also waits for
  `ro.persistent_properties.ready`. So the two-radio switch and a real STA
  connect (radio 2 as AP) are still untested.
- Note: `su` runs in the shell domain, so it cannot set `persist.vendor.maton.*`
  (correct policy); for tests I set it with a brief `setenforce 0`.
- Seen, harmless: modprobe `key write` denial while loading modules.
- Boot without the radio APEX: skipped for now (your call 08:50). Evidence so
  far: live130 booted with none of the radio services running, and the HAL
  VINTF fragments live inside the APEX, so no APEX means no declared HAL to
  wait for.

**live135 results** (enforcing):
- FAIL → fixed for 136 (656a541d): hwsim never loaded, so there was no wlan0.
  My 97019aa5 trigger waited on `ro.persistent_properties.ready`, which
  vendor_init may not read (denied). That condition was also never needed:
  init loads persist props (and waits for them) before apexd runs, and this rc
  is parsed from the APEX. So the "persist race" I blamed on live133 was
  wrong; the likely cause there is that my `setprop` never took. 136 retests
  `hwsim_radios=2` properly.
- With hwsim loaded by hand (`radios=2`, two phys) and the HAL restarted:
  - **PASS: hotspot.** The HAL created `wlan2` (AP) on wlan0's phy via
    nl80211, hostapd reported `AP-ENABLED`, and the other radio saw
    `MatonTest` at -30 dBm. Stopping the hotspot deleted `wlan2` again.
  - **PASS: Wi-Fi connect.** With a test AP `MatonNet` on the second radio,
    the framework STA (wlan0) associated: supplicant `COMPLETED`, 48 Mbps,
    `CTRL-EVENT-CONNECTED`. 0 denials, 0 crashes. No IPv4 because the test
    AP had no DHCP server.
  - Note: `cmd wifi connect-network` alone bails with "no requests" because
    the VM's ethernet satisfies the default request; `cmd wifi add-request`
    (pre-approved) works. On a laptop without ethernet this does not arise.
- Was "minor", turned out worse: after the nl80211 create, the HAL logs
  `Unknown iface name: wlan2`. The goldfish backend (used for every card)
  only lists wlan0/wlan1 as found at start, and passes the resulting null
  handle straight on as `Interface*`, so per-iface calls on a later
  interface could crash the HAL. **Fixed for 139 (33948688):** goldfish
  backend forked into `wifi/legacy_hal` as `libwifi-hal-matonos`, which
  lists every wireless netdev live; no AOSP change.

**live136:** `hwsim_radios=2` set (read back as 2), but after a reboot it is
empty again: the live image starts with a fresh `/data` every boot
(`/data/property` created at boot time, system settings files missing). So
any `persist.*` setting is lost on the live image, which is also what
happened on live133. Instead, on live136 I reloaded hwsim by hand with
`radios=2` (rmmod works now; the 126 EPERM is gone) and ran the switch:
- `persist.vendor.maton.wifi.hwsim_selectable=1` is needed: wifid refuses to
  select simulated cards otherwise (by design).
- **PASS: card switch.** wifid `select_device 02:00:00:00:01:00` renamed the
  old card to `wlan_int0` and the chosen one to `wlan0`; Wi-Fi then came up
  on the new card (framework STA on wlan0 = phy2), no crash.
- FAIL → fixed for 137 (12794e03): wifid was denied `nlmsg RTM_GETLINK`
  (kernel netlink_xperm). `if_nameindex()` failed, so the name collision check
  before a rename always said "free"; this switch only worked because no
  collision existed. Now allowed with `allowxperm … nlmsg RTM_GETLINK`, as
  AOSP dumpstate does.
- Also in 137: the Wi-Fi HAL death recipient sets `onUnlinked`, so the
  "will become an abort" warning is gone there too (the cookie is just a
  lookup key).
- Done: `switch_core.h` logs errors with `__android_log_print` on Android;
  `fprintf(stderr)` is only the host unit-test build.

**live137 results** (enforcing):
- PASS: no RTM_GETLINK denial; the Wi-Fi HAL no longer logs "will become an
  abort" (the remaining ones are stock: system_server, mediaswcodec,
  SystemUI, netd, adbd, statsd).
- PASS: card switch both ways (to card 2, then back to card 1 while `wlan0`
  is taken): names swap cleanly, 0 denials, 0 crashes.
- **FAIL → fixed for 138 (6c3620ac): Wi-Fi only turns on once.** After the
  first successful enable, WifiService caches the chip modes and calls
  `configureChip(3)` without asking `getAvailableModes` again. Every new chip
  object (any Wi-Fi off/on or HAL restart) still held the compile-time modes,
  so `configureChip` returned INVALID_ARGS without a log line and
  WifiSelfRecovery turned Wi-Fi off. Confirmed without any card switch: a
  fresh HAL process failed the same way. The HAL now loads wlan0's runtime
  mode in `configureChip` as well, and logs the rejection. The earlier tests
  missed this because each one did only the first enable.

**live138 results** (enforcing): the second-enable fix works.
- Wi-Fi off/on three times: on every time, no configureChip error.
- Two cards: switch to card 2 → on, back to card 1 → on, card 2 again → on;
  `wlan0` carries the chosen card's MAC each time, scans run. 0 denials,
  0 crashes.

**live141 results** (enforcing; build-141 = forked legacy backend):
- The backend creates handles for interfaces made later (`Adding interface
  handle for wlan2`); no "Unknown iface name" for wlan2.
- Open hotspot: PASS next to the STA connection (wlan2 UP, AP-ENABLED).
- **WPA2 hotspot: FAIL, kernel cause.** hostapd `set_key failed; err=-2`.
  Same with a hand-run hostapd on a bare hwsim radio, so not Android or the
  HAL. mac80211 asks for `ccm(aes)` through request_module, which Android
  cannot serve, and `CONFIG_CRYPTO_CCM=m`. After loading `ccm.ko` by hand
  the same config reaches AP-ENABLED. This breaks every WPA2/WPA3 (CCMP)
  link on real cards too, client side included. Fix 42440352 builds CCM
  into the kernel; the kernel is being rebuilt.
- Wi-Fi off/on three times: on every time.
- Card switch both ways via wifid: rename right (chosen card = wlan0,
  other = wlan_int0), Wi-Fi on again on each card with the right MAC.
  Note: `IChannel/wifi select_device` is served by wifid and only renames;
  the off/on around it is the bridge's WifiCardSwitcher (the Settings
  path), so the shell test re-enables by hand.
- 0 crashes; the only denials came from my shell under a hand-set permissive.

**Media APEX (user 12:00: "put sol on it"):**
- mediahal1 (sol): software Bluetooth audio provider fork `audio/bt-audio/`
  (A2DP, LE unicast + broadcast, hearing aid, HFP; PCM only, no offload),
  LE Audio profile properties, VINTF entry. Source-compiled only; merged
  2a9c0336, first built in 142. LE Audio still needs an ISO-capable
  controller; older adapters keep showing it unsupported.
- Webcam and network-speaker phases came back as notes only. mediahal2
  (sol, running) implements them: speakers via NsdManager in the bridge →
  `IChannel/audio` → HAL loads stock RAOP/Snapcast sink modules (no Avahi);
  then the PipeWire camera provider fork. ROC allowed (MPL-2.0; my first
  brief wrongly said permissive only).
- mediahal2 done 12:45, merged 06ccf770. Compile-checked only. Camera fork
  stays off (`MATON_PIPEWIRE_CAMERA=true` after a verified link). Speaker
  test needs a LAN-connected machine (QEMU NAT sees no mDNS). ROC not built.
  Acceptance steps: `audio/mediahal2.report.md`.

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

## 19:20 update
- build-152 (dt 3955d695, btd paren fix) running, ~70% at 19:19.
- In for build-153: 2689a752 System Bridge default-permissions (ACCESS_LOCAL_NETWORK + BLUETOOTH_CONNECT, kills the NSD picker popup), 9b9982e5 SystemUI caption icon/title fallback from topActivityInfo, b5b08ac4/030fa84c caption buttons = Lucide icons (octagon-x, square-plus, copy-plus restore, diamond-minus; ISC notice in caption/README.md).
- live150 VM (user's window): caption APK from 030fa84c installed by adb (built via server worktree /mnt/data/matonos/tmp/dt-caption, MATON_APPS_ONLY=matonos-caption). SystemUI half pending: `build.sh -m MatonSystemUI` after 152 ends, then push to VM.
- Open: boot animation shows stock AOSP "android" text on live150 (cause unknown); live150 rebooted itself once; adb shell hangs until `adb disconnect/connect`.

## 20:20 update
- Caption (wip/caption-look, merged 3575c57b): Lucide buttons 16 dp / 3-unit stroke
  (2 px at 160 dpi) in tonal circles 70% of caption height; WM Shell patch 0004 slot now
  fills the header height (fixes the stock strip under our caption).
- Settings: Lucide square-letter-m app icon + Android Settings tile (3f1d26f6);
  Developer section hidden until 5 taps on Build in About, GApps moved under it (ab66b31d).
  Both installed on live150 as app updates; taskbar icon may show the cached old M.
- build-152 (3955d695) finished OK but predates today's fixes, so it was skipped untested.
- build-153 (dt 3575c57b) started 20:15: test title/icon, no strip, boot animation,
  NSD picker, BT, denials; then keep only live-153.

## 22:55 update (night 11-25→26)

| Build | dt | Result |
|---|---|---|
| 154 | 917d92a7 | OK 22:47. live154 boots enforcing. **Boot animation = ours**: /oem/media/bootanimation.zip (matonos_bootlogo_file) played by BootAnimation. Gone: bridge netlink, vendor_modprobe key, kernel key, pc_gpu_detect sys_admin, regulatory.db, WirePlumber sysfs. Left: MediaProvider getattr/search on /oem/media (12×, harmless; mediaprovider_app is a private type, vendor policy can't name it), PipeWire fwmarkd write (Treble neverallow → dontaudit 29a6a8a5, in 156) |
| 155 | fdc52372 | running (started 22:48): first build after the apex_media split + Settings radio page |

- **Media APEX split (sol-low):** `audio/` = submodule https://github.com/MatonOS-dev/apex_media (public, 59 commits of history, 0 key markers, no GPLv3). build-hal.sh takes MATON_AOSP_ROOT/MATON_AUDIO_STAGE. Server: build-155.sh moves the old audio/ to logs/audio-pre-split once and copies the generated prebuilt dirs back.
- **Settings Hardware page (luna, b35f865c, merged 48b47705):** lists Wi-Fi cards / BT controllers (driver, MAC, virtual, active); select goes through the bridge's WifiCardSwitcher / BluetoothControllerSwitcher. wifid/btd snapshots gained `driver`. Open: the "present/virtual" status rows still say Unknown (HardwareModule.kt).
- **Settings icon:** the launcher's "Settings" is stock com.android.settings (gear). The MatonOS tile in stock Settings shows the Lucide M, on a grey background unlike the coloured stock tiles (your call). Launcher3 data was cleared on the VM → taskbar is back to default pins.
- **Decisions tonight:** microG spoofing stays until certification; LineageOS: no rebase. Low-prio backlog: installer-as-setup-wizard (setup.json, luna scope: out/pc-logs/agents/setupjson-scope.last.md), Seedvault, release-key signing check, security status page.
- VM: live154 only, headless, adb localhost:5560.

## 23:00 update
- f1a112eb btd: 1 s poll replaced with a NETLINK_KOBJECT_UEVENT listener (rescans on
  SUBSYSTEM=bluetooth events or ENOBUFS) + `netlink_kobject_uevent_socket` allow. Not
  compiled locally (no NDK on host); build-156 is the compile check.
- **Bluetooth with no controller (live154):** `cmd bluetooth_manager enable` → the HAL
  waits in waitHciDev, the stack aborts after 3 s (`hci_hal_impl_android.cc:103
  waitForInitialization: Can't start HAL`, a fixed 3000 ms on SDK ≥ 37), and it restarts
  5 times before the framework gives up and leaves the state OFF. Making the HAL fail
  fast in waitHciDev does NOT help: any failed init hits the same assert. A real fix
  means not enabling the stack when no controller exists (the BluetoothManagerService
  gate is an AOSP patch; hiding the QS/Settings toggles is vendor-side but apps can
  still call enable()). **Decision for the user.** Nothing changed.
- VM self-reboot: seen once on live150 (VM since deleted, no logs). Watching live154
  uptime instead.

## 23:25 update
- DeepSeek reviewed the radio page (report: out/pc-logs/agents/ds-review-radio/report.md).
  All 8 findings checked against the code and real; fixed in 38f40372 (hwsim/active rows
  not selectable, single switch at a time, BT active = bound, unbound title fallback,
  interface-name fallback for Wi-Fi, devices_changed refresh, bridge BT persist).
  Not typechecked locally; build-156 compiles it.
- build-155 (fdc52372, first after the apex_media split) at ~35% at 23:19.
- live154 uptime 29 min at 23:19 (since last boot); no self-reboot seen yet.

## 00:10 update (11-26)
- build-155 (fdc52372, first after the apex_media split) passed 00:04. live155 VM boots
  enforcing; denials only MediaProvider /oem/media getattr (known, private type); no
  crashes; pipewire + wireplumber + matonos-audio-hal up, AudioFlinger output thread
  present → the submodule split works. live154 VM + server live-154.img deleted.
- live154 had been up 1 h 16 min when I stopped it: no self-reboot seen.
- build-156 started 00:04 on dt 99b9b5f1 (fwmarkd dontaudit, radio-page fixes incl.
  review fixes, btd uevents). It's the first compile of the TSX and btd changes.
- VM launch: cmd.txt is one arg per line and needs
  `LD_LIBRARY_PATH=~/matonos/qemu-venus/lib` (virglrenderer 1.11):
  `LD_LIBRARY_PATH=… setsid nohup xargs -d '\n' -a cmd.txt env > qemu.log 2>&1 < /dev/null &`

## 00:45 update
- Low-prio backlog item done (main queue empty): tools/check-release-keys.sh flags any
  installed APK/APEX signed with AOSP's public test certs. Trial on server out dir
  (mid build-156, so not a clean run): 162 files, 1 hit — `system/apex/
  com.android.apex.cts.shim.apex` (AOSP prebuilt CTS shim, testkey-signed upstream; it
  carries no code, but an update signed with the public key could replace it).
  **User decision:** drop the shim from the image, or re-sign it with our key. Not
  wired into build.sh yet.

## 01:25 update
- build-156 (99b9b5f1) passed 01:19 → TSX radio fixes and btd uevent loop compile.
  live156 VM: enforcing, only the known MediaProvider getattr denials, no crashes.
  btd main thread waits in `__skb_wait_for_more_packets` (uevent recv) instead of
  nanosleep; no uevent denial. Hardware page: Wi-Fi "Virtual radio" + wlan0 row,
  Bluetooth "Not detected"; tapping the virtual wlan0 row makes no switcher/Wi-Fi call.
  live155 + server live-155.img deleted (live155 had been up 1 h 11 min, no self-reboot).
- luna `settings-security` started 01:2x: read-only Security page (SELinux, encryption,
  patch level, verified boot, build keys, ADB) in worktree ~/matonos/build/settings-security
  (wip/settings-security). Low-prio backlog item.

## 01:25b update
- Security page merged (fc77d9f0): luna's page (34d022be) + my 41fd2c07. The Settings APK is
  not platform-signed, so SELinux / encryption / vendor patch / verified boot come from a
  new read-only `hardware` → `security` bridge command (bridge policy: selinuxfs read +
  vendor_security_patch_level_prop); native module keeps patch level, build tags, ADB.
- build-157 started 01:23 on fc77d9f0 — first compile of the Security page.

## 02:20 update
- build-157 (fc77d9f0, Security page) still building at 02:19, no errors so far.
- live156 up 58 min, no self-reboot.
- luna `seedvault-scope` (report only, low-prio backlog): Android 17 branch, licences,
  Lineage/Calyx integration lines, storage targets, stub-app exclusion; report at
  out/pc-logs/agents/seedvault-scope.last.md.

## 02:35 update — Seedvault scope (luna report: out/pc-logs/agents/seedvault-scope.last.md)
- Use the `android17` branch (pinned 9d450d28); it has an in-tree Soong `android_app`
  (system_ext, privileged, `certificate: "platform"` = our platform release key, so luna's
  "signing blocker" is not one) + 3 XMLs (privapp perms, default perms, sysconfig) and
  a required `LocalContactsBackup` module.
- Licences (I checked): all 531 SPDX headers Apache-2.0; vendored jars Apache/MIT/BSD/
  MPL-2.0 (dav4jvm)/logback (EPL-1.0 or LGPL-2.1). GPL-3.0/AGPL-3.0 texts in LICENSES/
  are referenced by no file. **Open:** `libs/seedvault-chunker-0.1.jar` (CalyxOS,
  org.calyxos.seedvault.chunker) has no licence info → must be confirmed before adoption.
- Default transport: set `def_backup_transport` with a product RRO on SettingsProvider
  (no AOSP patch).
- MatonOS-specific: thousands of Linux stub APKs would be backup-eligible → need an
  exclusion; Flatpak data lies outside app-data backup.
- Not started; low priority, waits for the chunker licence.

## 02:50 update
- build-157 (fc77d9f0) passed 02:37. live157: enforcing, only known MediaProvider denials,
  no crashes. Security page shows SELinux Enforcing, Encrypted (file), patch 2026-06-05,
  Verified boot "Locked, verified", ADB Enabled — the bridge path works, no new denials.
  Vendor patch "Unknown" (prop really unset) and build keys "Test/development keys"
  (ro.build.tags = dev-keys, AOSP's tag for private non-test keys) → fixed in b1e4a672
  (VENDOR_SECURITY_PATCH := PLATFORM_SECURITY_PATCH; dev-keys shown as "Private keys").
- live156 + live-156.img deleted (live156 had been up 1 h 17 min, no self-reboot).
- build-158 started on b1e4a672.

## 04:05 update
- build-158 (b1e4a672) passed 03:55. live158: enforcing, only known MediaProvider denials,
  no crashes. Security page now complete: SELinux Enforcing, Encrypted (file),
  patch 2026-06-05, vendor patch 2026-06-05, Verified boot "Locked, verified",
  Build keys "Private keys", ADB Enabled. Security page item DONE.
- live157 + live-157.img deleted (live157 up 1 h 17 min, no self-reboot). live158 = the one image.
- Queue: only your decisions (Bluetooth no-controller loop; CTS shim APEX) and the
  setup.json installer (low priority, not started overnight).

## 06:20 final
- live158 up 2 h 23 min, no crashes, no new denials, no self-reboot.
- Waiting on you: (1) Bluetooth crash loop without a controller — AOSP gate in
  BluetoothManagerService vs hiding QS/Settings toggles; (2) CTS shim APEX — drop or re-sign.
- Next low-prio item: setup.json installer (not started). Seedvault blocked on chunker jar licence.

## 08:20 update
- live158 up 4 h 23 min, no crashes.
- BT crash loop explained to user: stock btlinux HAL waits forever for an HCI index;
  gd stack aborts after 3 s ("Can't start HAL") and only accepts SUCCESS from
  initializationComplete. Only loops while BT is enabled. Fix choice still open.
- CTS shim: user said re-sign; I recommended keeping the AOSP key (CTS installs its own
  shim v2/v3 against it; signing tools treat prebuilt APEX as PRESIGNED) and
  allowlisting it in check-release-keys.sh. Waiting for the answer; nothing changed.

## 09:55 update
- User: hide the Bluetooth toggle → 72a4d0cd (systembridge removes `bt` from
  sysui_qs_tiles while btd reports no controller, restores it on hot-plug; also turns BT
  back off if enabled without a controller). build-159 running (started 09:41).
- User: CTS shim = intended exception → check-release-keys.sh reports it INTENDED;
  server run: 162 files, 0 problems, exit 0.

## 11:00 update
- build-159 (72a4d0cd) tested on live159: BOTH halves ineffective → reverted (6dcce213).
  SystemUI rewrites sysui_qs_tiles from its in-memory list on any outside change
  (UserTileSpecRepository.kt "Settings is not the source of truth"); `cmd statusbar
  remove-tile` takes custom tile components only. `cmd bluetooth_manager enable` →
  4 crashes in BLE_TURNING_ON, before ACTION_STATE_CHANGED, then the framework gave up.
- No patch-free way to hide only the toggle. Options put to user: (a) declare
  android.hardware.bluetooth only when a controller exists at boot (AOSP's own
  "No Bluetooth Service (Bluetooth Hardware Not Present)" path; hides tile + Settings;
  dongle plugged after boot needs a reboot) or (b) a SystemUI/BluetoothManagerService patch.

## 21:20 update
- User picked (a) + DeepSeek analysis. DeepSeek hit its free-usage limit before
  reporting (out/pc-logs/agents/ds-bt-feature/report.md); design checked by hand.
- 67d98024: bluetooth/matonos-bt-detect.sh (ODM, domain matonos_bt_detect) runs as
  blocking exec_start in post-fs-data (before zygote-start). No /sys/class/bluetooth/hci*
  → writes /oem/etc/permissions/matonos-no-bluetooth.xml (<unavailable-feature>
  bluetooth + bluetooth_le, oemfs label; system_server already reads oemfs) → SystemServer
  skips the Bluetooth service. btd's own --boot step was not usable: it runs on
  apex.all.ready, which init queues after zygote-start (race).
- Dropped uninstalled VHCI-era bluetooth_features.xml; fixed stale rc/xml comments.
- build-160 running (policy-only check first, then image). Test: pm list features has no
  bluetooth, no BT tile, kmsg line from matonos-bt-detect, no new denials.
