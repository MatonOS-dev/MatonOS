# MatonOS (pc_x86_64) — bring-up notes

**MatonOS** (android → Automaton → maton): a free, donation-funded, AOSP-based
distro for generic x86_64 PCs. Say "based on AOSP", never "Android …"
(trademark). Vendor directory `device/maton`, brand/manufacturer `MatonOS`;
the product/lunch name stays the generic `pc_x86_64` (it is also the
`androidboot.hardware` value, fstab and init rc names).

AOSP (`android-latest-release`, `aosp_current` → `cp2a`) for generic x86_64
UEFI PCs, mainline kernel, Mesa graphics, installed by `tools/installer.sh`.

Layout: everything lives in `device/maton/pc_x86_64/` (the monorepo root).
Scripts are in `tools/`.

**When adding any service/HAL/script that init starts, follow the SELinux
checklist in `CLAUDE.md`** and run `tools/check-selinux-labels.sh` (build.sh
runs it after every AOSP build). Unlabelled vendor binaries are refused by
init even in permissive mode.

## Roadmap

**Top priority (user, 2026-09-24): get out of Soong's way.** Soong
re-analysis (20–40 min, the whole machine) stalls everything, so first:
1. the system bridge app + generic daemon channel;
2. our apps (Gradle) and daemons/HALs (NDK, incl. AIDL NDK backend) built
   outside Soong, imported by ONE stable prebuilts Android.bp — after that,
   day-to-day work never changes the Soong graph;
3. retiring AOSP patches (CLAUDE.md long-term goal list): own launcher via
   Gradle, input daemon (uinput) instead of inputflinger patches, Wi-Fi/
   Bluetooth spoofing instead of stubs, our own audio HAL instead of
   hardware/interfaces patches.
Feature work continues, but in this shape from now on.

- **v1**: basic loading, made **stable**: live image boots reliably to the UI
  on generic PCs (QEMU, build PC, Surface Pro 3).
- **v1.1**: F-Droid preinstalled (+ Privileged Extension) and the system app
  replacements (Fennec, Fossify suite, Open Camera) and the AOSP desktop windowing mode,
  updating from F-Droid's main repo. Our own repo/server pipeline stays v3.
  Also **Bluetooth detection**: v1 patches Bluetooth audio out of the audio
  HAL APEX (`patches/hardware/interfaces`) because not every PC has
  Bluetooth, and audioserver hangs waiting for any declared-but-absent
  `IModule`. v1.1 re-enables it only when a Bluetooth controller is present.
- **Status 2026-09-25 05:20: all AOSP patches dropped** (user decision: drop
  everything now, fix features back one by one without patches). The AOSP
  projects are reverted; `patches/` is empty; old files for reference in
  `retired-patches/`. drm_hwcomposer/minigbm run on our own `matonos/v1.2`
  fork branches. Features to restore without patches: stock AIDL audio HAL
  configuration + ODM ALSA endpoint glue, VM absolute pointers + PS/2 wake +
  desktop mode on keyboard+mouse (matonos-inputd). Boot-critical: audio policy
  config must name every IModule the stock HAL declares, including Bluetooth
  (the Bluetooth module can be empty to expose no routes).
- **Known gap (2026-09-25):** `fstab.pc_x86_64` (installed systems) now
  first-stage-mounts the ODM bundle; systems installed by the legacy
  `tools/installer.sh` have no `odm` partition and would not boot. Install
  me (v2) must create `odm_a/_b` (bundle/README.md); until then only the
  live image is supported.
- **v1.2 (before v2)**: sound, Wi-Fi and Bluetooth working, **and zero
  AOSP patches (user decision, 2026-09-24): `tools/apply-patches.sh` has
  nothing to apply** (the system bridge app is ours, not a patch). Patch →
  replacement → owner: BayLibre generic AIDL audio HAL + boot ALSA selector (audio);
  Wi-Fi stubs + fwb/0003 → spoofed Wi-Fi (wifi); Bluetooth stub + fwb/0005 →
  spoofed Bluetooth (bluetooth); frameworks/native 0001/0002 → input daemon
  via uinput (input); fwb/0002 desktop-on-keyboard+mouse → input daemon
  spoofs a virtual touchpad when a mouse is present so stock WM Shell logic
  applies (input); fwb/0004 maximise=fullscreen → fullscreen action in our
  launcher via a bridge method (launcher); fwb/0001 classic navbar +
  Launcher3 0001–0003 → obsolete with our launcher (launcher);
  drm_hwcomposer/minigbm patches → our own forks via the local manifest
  (vendorforks). If a replacement isn't possible without a patch, the
  feature is **dropped for now** (user rule) and listed below under
  "Dropped for zero patches". Earlier v1.2 detail: done by parallel Codex agents in `audio/`, `wifi/`, `bluetooth/`
  (briefs in `out/pc-logs/agents/`). **v2 starts after these.**
- **Hardware architecture (decided 2026-09-24)**: MatonOS owns the hardware,
  Android keeps its APIs. General principle in CLAUDE.md ("MatonOS owns the
  hardware glue"); applies to every hardware area, not only these:
  - **Audio (user decision 2026-09-25 ~21:00):** use BayLibre's generic AIDL
    audio HAL, pinned at `a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5`. A small
    asynchronous ODM selector picks the first `/proc/asound` card with a
    playback PCM (skipping Loopback/Dummy) and sets the HAL's primary card and
    device properties. Static schema-7.0 policy configures `default`,
    `r_submix`, `usb`, and `stub`; Bluetooth/LE is omitted. No detector-generated
    XML, bind mount, or `snd-aloop` load is shipped. If selection fails or no
    card exists, the HAL uses its paced stub properties. The upstream APEX is
    actually named `com.android.hardware.audio.generic` (not the README's
    `...baylibre`); use the built module name. This checkout's audio core is
    frozen at v4; upstream manifest is v3 and no AIDL v5 exists here, so ODM
    overrides to v4. Verify boot with HDA/no card and playback WAV.
  - **Wi-Fi / Bluetooth**: Android's stacks stay (wpa_supplicant/wificond/
    WifiService; Android Bluetooth + HCI HAL). Our own daemons (own SELinux
    domains) manage the devices underneath: enumerate adapters, choose which
    one Android gets (built-in vs USB dongle; later user choice from the v2
    settings app), rfkill power on/off, firmware, hotplug, and present the
    chosen one as `wlan0` / the HCI device the HAL opens, plus presence
    properties (`vendor.maton.{wifi,bluetooth}.present`).
- **One MatonOS Settings app, inlined into AOSP Settings (user leaning,
  2026-09-24)**: a single app (not one per area) whose pages are injected
  into the standard Settings via tile injection — activities with action
  `com.android.settings.action.IA_SETTINGS` + meta-data
  `com.android.settings.category` (`…category.ia.display`, `.ia.sound`,
  `.ia.connect`, `.ia.system`, `.ia.homepage`), title/icon/dynamic summary
  from our app; they open in Settings' two-pane layout on large screens.
  `SearchIndexablesProvider` puts our entries in Settings search; a
  framework overlay points "System update" at the Updater. No Settings
  patch. **Placement (user decision): one top-level "Hardware" entry on the
  Settings homepage (`…category.ia.homepage`)** opening our app's own page
  list: Display, Sleep, Audio devices, Wi-Fi/Bluetooth adapters, cameras,
  … (not scattered over Settings' categories). The Updater stays on
  Settings' "System update" entry (overlay). Install me stays separate (live image only). Ownership:
  `settings/` app shell (injection, search, style) by a Settings agent; each
  area delivers daemon + control API + its page in its own subpackage.
- **Own apps/daemons outside Soong (decided 2026-09-24)**: custom apps
  (Settings, v2 apps, launcher) are Gradle projects → APKs imported by fixed
  `android_app_import`; own native daemons built with the NDK → prebuilts.
  Keeps Soong analysis out of the day-to-day loop (see CLAUDE.md).
  Exception: one small platform-signed **system bridge** app in Soong
  exposes the needed hidden/private APIs via AIDL to trusted (privileged,
  allowlisted-certificate) callers only. It also forwards a generic
  `call/subscribe` channel (JSON over per-daemon Unix sockets
  `/dev/socket/matonos/<target>`) to our daemons, so new daemon features
  need no bridge change.
- **v2 Hardware settings features (user, 2026-09-24)**:
  - **Camera roles**: the user assigns how each physical camera is presented
    to apps — front ("selfie"), back ("world"), external, or hidden — and our
    camera HAL reports that facing/ID. Default: built-in laptop camera =
    front, extra USB webcams = external; IR/metadata nodes hidden.
  - **Audio device choice**: current selector picks the first playback-capable
    card at boot. User preference, analog-over-HDMI prioritization, runtime
    switching, and a Hardware → Audio control page are deferred.
- **Trusted developer apps (user idea, 2026-09-25)**: the system bridge keeps
  a revocable runtime trust list (package + signing cert + allowed targets)
  so developers can test system apps signed with their own key; granted via
  `adb shell cmd matonos-bridge trust <pkg> [targets]` or a Developer
  options page; only with Developer options on, persistent notification,
  logged; SYSTEM_BRIDGE gets the `development` protection flag for
  `pm grant`. Works on all builds (hobby project: users may do advanced
  things with their own devices); the user confirms each grant.
- **Shared app library startup check (user, 2026-09-25)**: all MatonOS apps
  use the shared MatonosClient library, which on startup checks the system
  bridge (installed/bindable, permission + trust, API version, required
  daemons) and otherwise shows one standard error dialog explaining the fix
  ("needs MatonOS" / "update MatonOS" / "dev build not trusted: adb shell cmd
  matonos-bridge trust <pkg>") and exits cleanly. A developer option "Allow apps to request
  bridge trust" (off by default) adds a "Trust this app…" button that opens
  the Trusted developer apps settings page prefilled for that app; the user
  confirms there (like "Install unknown apps"). The library also has
  `BridgeMode.OPTIONAL` (no dialog; isAvailable()/callbacks; clients degrade
  to "unavailable") for third-party apps that use MatonOS features only when
  present; our own apps use `REQUIRED`. `MatonOS.requestTrust(activity, targets)` lets any app
  bring up the trust page itself (e.g. an "Enable MatonOS features" button)
  and get the outcome back.
- **Bridge ban list (user, 2026-09-25)**: banned apps (package+cert or
  package) get all bridge calls refused and see "bridge not present" via the
  client library (for bad actors and apps that break when they detect
  MatonOS); user list + a MatonOS default list shipped with point updates,
  user overrides win. The bridge is a hidden system app: no launcher entry,
  and NOT `forceQueryable`, so Android's package visibility (API 30+) hides
  it from every app that doesn't declare `<queries>` for it or hold
  QUERY_ALL_PACKAGES; MatonosClient adds the `<queries>` entry via manifest
  merge for our apps. Only apps that explicitly look for it can see the
  package; the ban list covers them.
- **Boot animation (user, 2026-09-25)**: "MatonOS" wordmark in Montserrat
  (OFL) with the stock shimmer effect (shine scrolling behind the wordmark
  mask, white on black), as a generated `bootanimation.zip` in
  /product/media (bootanim/generate.py). One-time product copy rule added at
  the next deliberate Soong graph change.
- **v2 apps (decided 2026-09-24)**, each an ordinary app over a small native
  service (architecture principle below):
  1. **Sleep control**: app/service that feeds Android's state to
     matonos-sleepd (see "v2: Android ↔ sleepd bridge" below) and exposes the
     sleep settings (idle timeout; **sleep mode menu** listing only the modes
     the hardware supports — sleepd reads `/sys/power/mem_sleep` (s2idle,
     deep/S3) and applies the saved choice before each suspend; no hibernate).
  2. **Install me**: copies the running live system to a drive (**whole drive
     only**, wiped; no dual boot in v2) as an **A/B** install, then removes
     itself from the installed copy (the installed system never shows it).
  3. **Updater** (moved from v3): fetches and downloads OS updates from
     **han-mc-server** (static HTTPS feed) and applies them with
     `update_engine` to the inactive slot (A/B design under v3 below, now v2).
- **v2**: install app: Android-based installer (live image installs itself);
  built together with the user, starting right after v1 testing.
  Design sketch: privileged system_ext app (UI only, platform-signed) +
  native install service in its own SELinux domain with block-device access,
  AIDL interface guarded by a signature|privileged permission (apps can't
  open /dev/block/* whatever their permissions; same split as gsid/DSU).
  Service: sgdisk + newfs_msdos (both in AOSP), copy the running live
  `super` and grow its LP metadata with liblp to the 8 GiB install super,
  copy ESP contents, write loader entries using androidboot.boot_part_uuid
  (replaces installer.sh's sysfs boot_devices). Refuse the live medium.
  Open: UEFI boot entry (efivarfs vs fallback BOOTX64.EFI), SELinux policy,
  progress/cancel before partitioning.
  **Displays follow the hardware-glue principle (decided 2026-09-24)**: a
  MatonOS display daemon (own SELinux domain) changes on the fly what
  display hardware Android sees, and Android just handles ordinary hotplugs.
  Kernel hooks, no Android patch expected: force connectors on/off via
  `/sys/class/drm/<card>-<connector>/status` (emits a hotplug uevent →
  drm_hwcomposer re-reads → SurfaceFlinger adds/removes the display);
  limit/fix the offered modes and physical size (density) with debugfs
  `edid_override` (trimmed/corrected EDID); primary monitor via
  drm_hwcomposer's `vendor.hwc.drm.primary_display_order`; UI scale via
  forced density. To verify: debugfs availability in our domain, hwc
  reaction to forced status/EDID changes, SurfaceFlinger mode switch.
  The v2 display settings app becomes a front end for this daemon.
  **Cameras** work the same way (dynamic): our daemon decides which video
  devices Android's External Camera HAL gets (filter IR/metadata nodes,
  choose front/back, USB webcam hotplug). **Sensors, battery, lid** etc. are
  static: detected once at early boot, then fixed HAL config/properties.
  Earlier sketch, superseded where it conflicts: **display settings app** (privileged system app, no native
  service): resolution/refresh via DisplayManager.setGlobalUserPreferredDisplayMode
  (modes come from drm_hwcomposer/EDID), UI scale via forced density
  (overriding the pre-boot EDID default), optional lower render resolution.
  Check first what AOSP's external-display settings (desktop mode) already
  provide with drm_hwcomposer.
  Also v2: **"Network debugging" option** (Developer options, off by
  default): adb over any interface incl. Ethernet, reusing Android's
  Wireless-debugging pairing + TLS (stock Wireless debugging is Wi-Fi only).
  Never plain unauthenticated `adb tcpip`. v1 debug: userdebug starts adbd
  with ro.adb.secure=1 (auth prompt on screen); serial root shell +
  tools/qemu-shell.py until the UI is up.
- **Release model (user, 2026-09-25)**: users see only MatonOS versions,
  never the Android version. A **release** per patch day (bi-yearly, aligned
  with AOSP source drops): `YY.MM` — `26.12`, `27.6`, … — whether it's
  rebased on the same Android version or a new one is internal. **Rolling
  point updates** of the current release (`26.12.1`, `26.12.2`, …) install
  normally via the Updater. A new release is OFFERED as "Upgrade from 26.12
  to 27.6" (release notes, "later", repeating, never forced; same A/B
  update_engine path, old slot = fallback). **Upgrade to get fixes**: once a
  new release ships, the old one gets no more point updates. Feed: only the
  current release carries point updates, plus an "upgrade available" record
  for older releases.
- **v3: installer gets more options** (user, 2026-09-25; details TBD — e.g.
  dual boot next to another OS, custom partition sizes, ESP/boot-entry
  choice, repair/reinstall keeping user data, encryption choices). v2's
  install service must be built extensibly (options as a versioned
  request object over the channel, the UI as steps).
- **v3** (OS updates moved to v2; v3 keeps system-app updates via our own
  F-Droid repo). OS update design, now built in v2: reuse AOSP `update_engine` (SELinux-confined,
  downloads signed payload.bin from our server, writes inactive slot) + a
  client app. Needs A/B: "A/B with dynamic partitions" (both slots fit in
  the 8 GiB super; no Virtual A/B, whose dm-user is ACK-only), kernel +
  ramdisks in two XBOOTLDR partitions (boot_a/boot_b, FAT) that systemd-boot
  reads natively, systemd-boot boot counting for automatic fallback, and a
  custom boot-control HAL driving the loader entries + androidboot.slot_suffix.
  Natural point to enable AVB.
  **OS updater** (v2): system app; LineageOS's Updater (Apache-2.0: JSON feed →
  download → update_engine) is a good base. Feed + OTA files on
  **han-mc-server** as static HTTPS (decided 2026-09-24). Needs real
  release keys (not AOSP test keys), kept off the build machine.
  **System app updates between OS releases** (Play can't host them;
  F-Droid itself is preinstalled in **v1.1**; **v3** adds our repo as a
  ROM-default repo and the server pipeline; v2 apps ship only inside the
  OS image until then):
  preferred = own F-Droid repo (fdroidserver, signed index, static hosting;
  **hosted on han-mc-server**, which also signs the repo index (monthly
  WebView updates happen there; see key rule), served by Caddy/nginx; LAN-only
  for testing, HTTPS + domain before any public use)
  + preinstalled F-Droid client with Privileged Extension (silent installs;
  also covers third-party apps). Fallback = small privileged updater app
  (JSON index, PackageInstaller sessions). Rules from v2 on: sign our apps
  with per-app keys (not the platform key) and grant privileges via the
  privapp allowlist; keep native services thin (only OTA can update them);
  app updates can't gain new privileged permissions (allowlist is per OS
  release).
  **Decided: installed systems use A/B from v2** (super for two slots +
  boot_a/boot_b); the live image stays single-slot. One A/B build serves
  both: make-live.sh repacks only the _a partitions and passes
  androidboot.slot_suffix=_a. v1 stays non-A/B; installer.sh/make-payload.sh
  become legacy once the v2 app replaces them.
- **v5 (user, 2026-09-25): Bluetooth audio (classic + LE audio) via OUR Bluetooth HAL** — it provides the Bluetooth audio provider behind the stable virtual controller, and only then does the audio policy gain the bluetooth module. Until v5: no Bluetooth audio anywhere in the shipped policy (it blocked boot).
- **v5 (far future, user 2026-09-25): cellular for PCs with built-in modems**
  (WWAN M.2 LTE/5G, MBIM/QMI). Likely our own Radio HAL (IRadio AIDL →
  MBIM/QMI), possibly over a ModemManager-like usermode driver in ODM; no
  modem → no radio. TeleService/CarrierConfig are kept now for that reason;
  the SIM/carrier apps dropped in v1.2 (Stk, SimAppDialog,
  CarrierDefaultApp, ImsServiceEntitlement) return with it. eSIM: only
  with hardware (modem eUICC or removable eSIM cards like eSIM.me/5ber),
  managed by Android's EuiccManager + an open-source LPA (OpenEUICC /
  EasyEUICC) via APDU passthrough in our Radio HAL. A software "virtual
  eSIM" is impossible (GSMA-certified eUICC keys required).
- **v4 decision (user, 2026-09-25): sockets, not shipped binaries.** MatonOS
  ships NO ARM translation and NO Google software; the system bridge offers
  validated hooks the user fills:
  1. **Native bridge (ARM→x86)**: accepts any user-supplied compliant native
     bridge (x86_64 ELF exporting AOSP's `NativeBridgeItf` + its ARM runtime
     libs, e.g. libndk_translation/houdini obtained by the user). Checks:
     well-formed x86_64 ELF, `NativeBridgeItf` present with a version this
     Android supports, all callbacks non-null, probe calls
     (isCompatibleWith, getAppEnv, isSupported…) return sane values,
     companion lib set complete. Installed as an add-on, enabled
     (`ro.dalvik.vm.native.bridge` etc.) on next boot. Open: add-on storage
     zygote may load from + setting the props without Soong.
  2. **GMS replacement (microG etc.)**: an interface to install such
     packages privileged, grant their declared permissions, register them as
     providers. **Signature spoofing (decided 2026-09-25, v4): the ONE allowed AOSP
     patch** — a minimal (~40-line) hook in PackageManagerService that only
     asks the system bridge "may this app claim this certificate?"; all
     policy, consent, UI and revocation live in the bridge; inactive unless
     the user enables a GMS-replacement package through the bridge.
     (Taking over PMS from the bridge is impossible: PMS lives inside
     system_server and the framework calls it in-process.)
- **v4**: Google Play via the add-on slot (stub/overlay), maybe the Linux
  sandbox, and turning it into a real little distro. Also: libcamera
  (MIPI/IPU cameras) and Aurora Store (optional, e.g. offered via the same
  add-on/first-boot choice as Play).
  Linux sandbox sketch: native `linuxd` service (own SELinux domain) mounts
  a distro rootfs from /data, sets up namespaces/cgroups, runs Linux
  processes in a confined domain (render node, own files, no binder).
  Wayland bridge = ordinary SDK app acting as compositor: one Android
  task/Activity per Linux toplevel, dma-buf import via
  EGL_EXT_image_dma_buf_import, .desktop files -> launcher shortcuts; IME,
  clipboard, shared files, PipeWire<->AAudio to design. Prior art:
  Termux:X11, ChromeOS Exo/Sommelier.

Architecture principle (from v2 on): privileged work lives in small native
services with their own SELinux domains, exposed over AIDL (`aidl_interface`,
generating a Java/Kotlin client library) and guarded by Android permissions.
Apps stay ordinary SDK apps, so the standard Android dev tools apply.

## TODO (user reminders)

- **Enable NVK in our Mesa builds** (user, 2026-09-25): add `nouveau` to
  `-Dvulkan-drivers` in tools/build-mesa.sh (NVIDIA Vulkan on the nouveau
  kernel driver) and use Zink on NVK for GLES on NVIDIA. Needs Rust
  cross-compilation for the NDK target (x86_64-linux-android: rustc +
  bindgen matching Mesa's minimum) because NVK's compiler (NAK) is Rust.
  pc-gpu-detect.sh: pick `ro.hardware.vulkan=nouveau` (and Zink for GL) on
  NVIDIA; Turing+ needs GSP firmware (already collected via modinfo).
  **NVIDIA generations (user, 2026-09-25)** — the nouveau path must still
  work for pre-20-series cards:
  - Turing+ (RTX 20+, GTX 16): full reclocking via GSP → nothing special.
  - GTX 900 (Maxwell 2), 10-series (Pascal), Volta: nouveau can't reclock
    (no signed PMU firmware) → stuck at boot clocks → show a SYSTEM WARNING
    with exactly this text (user): "Incompatible GPU - Performance will suffer".
  - Legacy GTX 600/700 and early 700/800 mobile (Kepler, plus Maxwell 1
    GM107/GM108 e.g. GTX 750/750 Ti): UNSUPPORTED (user, 2026-09-25) —
    no pstate forcing; just a notification on boot in the LIVE environment
    that the card is unsupported (so users know before installing).
  Implementation: early-boot GPU detection (pc-gpu-detect.sh → our GPU/display
  usermode driver): PCI ID → `vendor.maton.gpu.class` (full|noreclock|
  legacy); the warning/notification is posted by our
  Settings app / a bridge notifier reading the GPU state via the channel,
  linking to a Hardware → Display/GPU info page.

- **Debug boot entry without the boot logo (user, 2026-09-25)**: the
  "MatonOS Live (debug)" entry should show the kernel/init log on screen
  (no boot animation): e.g. `console=tty0` + our ODM init rc setting
  `debug.sf.nobootanimation=1` when `ro.boot.matonos.debug=1`
  (androidboot.matonos.debug=1 in the debug loader entry), so a hang shows
  where it happens.
- **V3: MatonOS under the motherboard's EFI logo (user request)**: keep the
  firmware logo (ACPI BGRT: /sys/firmware/acpi/bgrt/image + xoffset/yoffset)
  on screen through boot and show "MatonOS" (+ progress) beneath it, like
  Windows does — e.g. the boot animation renders the BGRT image at its
  firmware position with our wordmark below; kernel keeps the firmware
  framebuffer quiet (no fbcon text, no mode flash) until SurfaceFlinger.

## Dropped for zero patches (v1.2)

Features given up until they can be done without patching AOSP (user rule).
Each entry: what, why no non-patch way, what it would need.
- **Input: Android listing only the virtual keyboard/pointer.** matonos-inputd
  grabs physical keyboards/pointers (EVIOCGRAB) and re-emits them through
  stable uinput devices, but stock EventHub still lists the (now inert)
  physical devices in its inventory. Hiding them needs framework support
  (or a reliable permission/label scheme for physical vs uinput event nodes,
  which ueventd can't tell apart at creation). Events work; only the device
  list shows extras. Revisit: ueventd/SELinux trick, or upstream proposal.

## Decisions

| Area | Decision | Why |
|---|---|---|
| Lunch | `pc_x86_64-aosp_current-userdebug` | `aosp_current` alias exists in `build/release/release_config_map.textproto` |
| ABI | 64-bit only (`core_64_bit_only.mk`) | Avoid building Mesa/HALs twice; same as cuttlefish `x86_64_only` |
| Arch variant | `x86_64` (baseline) | Widest PC compatibility |
| Hardware description | ACPI + PCI enumeration, **no DTB** | x86 firmware provides ACPI; nothing like `dtb.img` is needed |
| Kernel | `TARGET_NO_KERNEL := true` | Built out-of-tree by `build-kernel.sh`, packaged by `make-payload.sh` |
| Ramdisks | `BOARD_BOOT_HEADER_VERSION := 4` | Only way to get `vendor_ramdisk.img` built; `vendor_boot.img` is produced but not shipped |
| Kernel cmdline | Lives in `make-payload.sh` → `payload.conf` → systemd-boot entry | `BOARD_KERNEL_CMDLINE`/`BOARD_BOOTCONFIG` only go into `vendor_boot.img`, which we don't use |
| Partitions | GPT `esp, misc, metadata, super, userdata`; no boot/recovery/vbmeta | Contract of `installer.sh` |
| super | 8 GiB, one group `pc_dynamic_partitions` (super − 4 MiB): system, system_ext, product, vendor | `BOARD_SUPER_PARTITION_SIZE` == `SUPER_SIZE_BYTES` (8589934592) |
| RO fs | erofs | Smaller super; erofs is in mainline |
| AVB | off | No verification in the boot chain |
| SELinux | policy enforced by build, `androidboot.selinux=permissive` on cmdline | Only honoured on userdebug/eng |
| Encryption | FBE v2 `aes-256-xts:aes-256-cts:v2`, no `inlinecrypt` | No inline crypto engines on PCs |
| Metadata encryption | **off** (no `keydirectory=`) | Needs `dm-default-key`, which is Android-common-kernel only, not mainline |
| Userdata checkpoint | **off** (no `checkpoint=block`) | Needs `dm-bow` (ACK only); also irrelevant for non-A/B |
| Module loading | ueventd `modalias_handling` from `/vendor/lib/modules` | PC hardware varies; storage is built in so first stage needs no modules |
| HALs | software keymint (rust), gatekeeper, health example, power, thermal, usb, dumpstate, audio default | All from `hardware/interfaces`; names checked against their `Android.bp` |
| VINTF | `target-level="202604"`, `PRODUCT_SHIPPING_API_LEVEL := 37` | Same as cuttlefish on this branch |
| Kernel version | Mainline stable **7.2.y** in `~/Documents/linux` (6.18 kept in `~/Documents/linux-6.18`) | 7.x has the SELinux `memfd_class` policy capability (6.18 doesn't) and newer GPU/laptop support; user runs 7.2 on this hardware. Mainline, not ACK (decided). Plan: move to the 2026 LTS once announced |
| Target hardware | **Generic x86_64 UEFI PCs**, never tailored to the build machine | Broad driver coverage is the reason for using mainline |
| Kernel config | `kernel/base.config` (PikaOS 7.2.6 distro config, ~6.3k modules) + `kernel/configs/b/android-6.12/android-base.config` + `kernel/pc.config` | Distro config = broad hardware coverage. `pc.config` holds only Android/boot essentials and overrides (built-in storage/erofs, SELinux in LSM, no module compression, no debug info/BTF). `c/android-6.18` fragment is an empty placeholder |
| Module signing | `MODULE_SIG`+`MODULE_SIG_ALL` with a build-generated key, not enforced | Forced on by `SECURITY_LOCKDOWN_LSM`; first step toward protected add-on modules. Key regenerates on clean kernel builds: persist it before enforcing |
| Suspend | s2idle and S3 both built; state chosen by firmware default or `mem_sleep_default=` | Generic; platform PM drivers (AMD PMC, Intel PMC, pinctrl wake) come from base config |
| Kernel toolchain | AOSP prebuilt clang (`LLVM=1`) with `-Wno-gcc-install-dir-libstdcxx`, host clang fallback | Reproducible. Without the flag, AOSP clang's host-GCC warning + kbuild's `-Werror` probes silently disabled STACKPROTECTOR, RETHUNK etc. |
| Binder | **Rust binder** (`CONFIG_RUST`, `ANDROID_BINDER_IPC_RUST`; C driver off, mainline makes them exclusive), own binderfs; `ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"` | First boots: an empty device list meant /dev/binderfs had no nodes, so init.rc's /dev/binder symlinks dangled and every service aborted. Rust binder needs rustup toolchain (build-kernel.sh adds ~/.cargo/bin) |
| MODVERSIONS | off | Kernel Rust requires !MODVERSIONS or GENDWARFKSYMS (needs debug info, which is off); kernel + modules always built together |
| cgroup v1 cpuset | `CPUSETS_V1=y` | Android mounts v1 blkio/cpu/cpuset (all required); v1 cpuset is optional since 6.12 and was off → SetupCgroups failed, reboot loop on first boot |
| iptables | `NETFILTER_ADVANCED`, `NETFILTER_XTABLES_LEGACY`, `IP(6)_NF_IPTABLES_LEGACY` =y | netd uses legacy iptables; mainline made these opt-in, which dropped ~50 android-base options |
| FW_CACHE | stays =y (android-base wants n) | nouveau selects it for suspend/resume |
| Modules | `modules_install INSTALL_MOD_STRIP=1`, flat in `prebuilt/modules` → `BOARD_VENDOR_KERNEL_MODULES` | AOSP runs depmod → `modules.alias` for ueventd (`/vendor/lib/modules`) |
| Firmware | Files named by `modinfo -F firmware` of our modules, from a sparse linux-firmware clone, zstd-compressed → `/vendor/firmware` | Covers all built drivers; `.zst` loaded natively (`FW_LOADER_COMPRESS_ZSTD`) |
| OTA | `PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false`; no AOSP OTA packages | Kernel is external and can't meet ACK-only VINTF kernel requirements. OTA will probably be a custom mechanism later (it would have to update ESP files and `super`) |
| Firmware path | `firmware_class.path=/vendor/firmware` added to `make-payload.sh` CMDLINE | Direct loader only searches `/lib/firmware` otherwise; no usermode helper |
| v1 deliverable | **Live image** `pc_x86_64-live.img` (raw GPT disk image, dd to USB), not an ISO; installer later | Linux doesn't expose partitions inside an ISO booted from (virtual) CD, so `super` would be unreachable |
| Live boot disk | `androidboot.boot_part_uuid=<ESP PARTUUID>` (random per image) | Works on any port; matches SCSI (USB)/NVMe/MMC, **not virtio-blk** |
| Live data | `/data` + `/metadata` on brd RAM disks (`brd.rd_nr=2`), `fstab.pc_x86_64.live` via `androidboot.fstab_suffix` | Nothing persists; installed-system fstab unchanged |
| Live super | Repacked with `lpmake` to actual size | Fits smaller sticks than the 8 GiB install super |
| Mesa | Mesa 26.2.3 built with NDK r30 (API 35) via `tools/build-mesa.sh`, staged as vendor prebuilts. GL: iris, radeonsi (ACO, no LLVM), virgl, zink, nouveau, softpipe. Vulkan: anv, radv, venus | AOSP's mesa3d only builds lavapipe + gfxstream. Intel needs host `mesa_clc`/`vtn_bindgen2` built against LLVM 21 |
| Vulkan HAL choice | `pc-gpu-detect.sh` at early-init sets `ro.hardware.vulkan` from the primary GPU's PCI vendor | Loader loads one HAL; hardware varies. Grow this into the general per-hardware config step |
| gralloc | **BlissOS/android-generic minigbm** (`external_minigbm`, branch `14-x86`) via `manifest/maton.xml` (repo local manifest). Its `gbm_mesa` backend allocates through Mesa's GBM for every Mesa GPU (proper tiling on AMD/Intel/NVIDIA). `patches/external/minigbm` makes the AIDL allocator + stable-C mapper use it (`minigbm:backend=gbm_mesa`) | User's choice. AOSP minigbm's amdgpu backend needs Mesa's retired DRI image API; BlissOS's gbm_mesa only had HIDL gralloc4 front ends, which FCM 202604 doesn't allow |
| Mesa ↔ gralloc | `gralloc.minigbm_gbm_mesa` + `ro.hardware.gralloc=minigbm_gbm_mesa` for Mesa's `cros` u_gralloc backend | NDK-built Mesa has no IMapper support |
| GBM | Mesa built with `-Dgbm=enabled` → `libgbm_mesa.so` + `gbm/dri_gbm.so`; `libgbm_mesa_wrapper.so` built from minigbm's `gbm_mesa_driver/` by `build-mesa.sh` | minigbm's gbm_mesa backend dlopens the wrapper |
| libelf | Built by `build-mesa.sh` from AOSP `external/elfutils` (zstd off, vanilla zlib 1.3.1 linked in), shipped as shared `libelf.so` | radeonsi requires libelf; shared keeps LGPL compliance simple |
| UI rendering | GL (`ro.hwui.use_vulkan=false`, RenderEngine skiaglthreaded); no Vulkan feature XMLs | Vulkan availability varies per PC |
| AOSP patches | `patches/<project>/*.patch`, applied by `tools/apply-patches.sh` (idempotent) | `repo sync` needs them reverted first |

## Open issues / to verify

- [x] **Sync incomplete** when task 1 was written (`system/`, `prebuilts/`,
      `packages/` missing). Once done: `lunch pc_x86_64-aosp_current-userdebug`
      and `m nothing` / `m vendorramdisk ramdisk` to validate.
- [x] fstab lookup (`system/fs/fs_mgr/libfstab/fstab.cpp` `GetFstabPath`):
      suffix from `androidboot.hardware`; searches `/vendor/etc/fstab.*`
      then `/fstab.*`, so vendor-ramdisk root works for first stage.
- [x] Blank `/metadata`: first-stage mount skips a formattable partition
      it can't mount; second-stage `mount_all --early` (our `on fs`) formats
      it when `partition_wiped()` (installer zeroes the first 16 MiB). No
      installer change needed.
- [x] `modalias_handling` and `firmware_directories` are valid ueventd
      keywords (`system/core/init/ueventd_parser.cpp`).
- [x] ashmem: libcutils only uses memfd with the `memfd_class` SELinux
      capability + vendor API >= 202604 + app target SDK >= 37; otherwise
      `/dev/ashmem`, which mainline lacks. Set `sys.use_memfd=true`.
      Revisit memfd SELinux labelling before going enforcing.
      `sys.use_memfd` is still needed on 7.x: `memfd_class` only switches
      apps targeting SDK >= 37; older apps would still open `/dev/ashmem`.
      Effects of forced memfd: ashmem pin/unpin (purgeable memory) is a no-op;
      apps opening `/dev/ashmem` or issuing ashmem ioctls directly fail (rare);
      meminfo/maps show `memfd:` instead of `/dev/ashmem`.
- [ ] Kernel must decompress the ramdisk compression used (gzip by default →
      `CONFIG_RD_GZIP`), and systemd-boot concatenates the two initrds.
- [x] Firmware path: `firmware_class.path=/vendor/firmware` added to
      `make-payload.sh` CMDLINE (installer takes it from payload.conf).
- [ ] android-base options with no mainline 7.2 equivalent (ACK-only or
      removed upstream): `ASHMEM`, `DM_DEFAULT_KEY`, `UID_SYS_STATS`,
      `CPU_FREQ_TIMES`, `NETFILTER_XT_MATCH_QUOTA2(_LOG)`; removed upstream:
      `BPFILTER`, `NF_CT_PROTO_DCCP`, `NF_CT_PROTO_UDPLITE` (7.x), `SCHED_DEBUG`, `USELIB`,
      `ANDROID_LOW_MEMORY_KILLER`, `ANDROID_PARANOID_NETWORK`.
      Watch for: netd `xt_quota2` (data-usage alerts) failing, battery-stats
      per-UID CPU/IO numbers missing. Expected to be non-fatal.
- [ ] android-base disables `SYSVIPC` and `FHANDLE`; a future glibc Linux
      container would want SYSVIPC (X11 MIT-SHM etc.).
- [ ] Firmware for ~6k modules is large (nouveau GSP, amdgpu, iwlwifi,
      ath, mediatek...). Check vendor image size vs the 8 GiB super.
- [ ] Suspend on real hardware: Android only turns the screen on for keys
      flagged WAKE; verify that a USB keyboard/mouse wake actually wakes the
      UI (may need keylayout/idc or framework config).
- [x] Host packages installed: flex bison libelf-dev libssl-dev pkg-config
      dwarves zstd shellcheck qemu-system-x86 ovmf gdisk dosfstools mtools
      systemd-boot-efi zip. (`sgdisk`/`mkfs.vfat`/`modinfo` live in
      /usr/sbin, not on the user PATH; scripts add it.)
- [ ] Build host has 23 GB RAM + 30 GB swap and ~200 GB free; AOSP `out/`
      will be tight on disk.
- [ ] Graphics untested: Mesa NDK build, minigbm `pc` patch compile, u_gralloc
      cros backend with minigbm, drm_hwcomposer on simpledrm/amdgpu/i915.
- [ ] `repo sync` of external/minigbm now follows android-generic 14-x86;
      re-run `tools/apply-patches.sh` after syncing. Undo: delete
      `.repo/local_manifests/maton.xml`, `repo sync --force-sync external/minigbm`.
- [ ] Mapper runs in every app process (sphal) and dlopens the GBM wrapper →
      libgbm_mesa → dri_gbm → libgallium: check memory cost per process.
- [ ] No GL fallback check yet for GPUs without a Mesa driver (QEMU std
      VGA, old hardware): softpipe via EGL may need forcing.
- [x] Rust for NVK (user-level rustup, ~/.cargo/bin): rustc 1.98.1 with
      target x86_64-linux-android + rust-src, bindgen 0.73.2, cbindgen
      0.29.4 (Mesa 26.2 needs rustc >= 1.85, bindgen >= 0.71).
- [ ] Host needs for Mesa: meson ninja-build glslang-tools python3-mako
      python3-yaml spirv-tools clang-21 llvm-21-dev libclang-21-dev
      libclang-cpp21-dev libclc-21-dev libllvmspirvlib-21-dev libpolly-21-dev.

## Base: AOSP (decided)

LineageOS was considered (security merges, Updater, signing tooling) and
rejected in favour of staying on plain AOSP; its ideas (Updater app, F-Droid
Privileged Extension) are still reused where useful.

## Security updates

- AOSP: monthly Android Security Bulletin (patch levels YYYY-MM-01 / -05);
  merge the AOSP security tags monthly/quarterly. Mainline modules and
  WebView don't get Play updates here: they update only with our OS.
- Kernel: follow mainline stable (7.2.y) closely; move to the 2026 LTS.
- Mesa/minigbm/drm_hwcomposer: bump with OS releases.
- Apps (Fennec, Fossify, …): F-Droid updates them independently (v3).
- **System WebView (decided): ships with the OS** (AOSP's built-in
  Chromium WebView), updated with OS releases. Chromium has frequent
  exploited bugs, so use point releases (e.g. 26.12.1) to pick up important
  WebView updates from AOSP's external/chromium-webview between releases.
  **Monthly WebView job (v3, `update-webview.sh`) runs on han-mc-server**
  (build PC only does the twice-yearly releases). WebView is signed with
  the "MatonOS updatable apps" key via `PRODUCT_CERTIFICATE_OVERRIDES`
  (module otherwise uses the default dev cert shared with many system
  apps). Job
  (systemd timer): shallow partial clone of external/chromium-webview with
  only the newest x86_64 APK checked out (full history is many GB; server
  has ~26 GB free) → if newer: apksigner with the WebView key, check
  versionCode increased → `fdroid update` + publish. (Signs with the
  updatable-apps key.) Server needs apksigner,
  fdroidserver, a JRE. Build PC bumps RELEASE_PACKAGE_WEBVIEW_VERSION at
  each OS release.
  **Generalised (v3): server-updatable system apps** = monthly pipeline on
  han-mc-server for system apps that are (1) not platform-signed / not
  sharedUserId system (those are "critical": update only with OS
  releases), (2) not APEX/Mainline modules, (3) not already updated by
  F-Droid's main repo. Sources: prebuilt APKs (WebView: download + re-sign)
  and our own apps, which from v2 on are Gradle projects so the server can
  build them with just the Android SDK (the OS build ships the same code).
  AOSP apps built from source (Launcher3, DocumentsUI, …) need the AOSP tree
  → only at the twice-yearly releases. Keys (decide before first v3
  release; app keys can't change later without APK key rotation):
  **decided: one shared "MatonOS updatable apps" key for WebView and all
  server-updated apps**, set per module via PRODUCT_CERTIFICATE_OVERRIDES.
  **Key rule:** OS keys (platform, releasekey, media, shared, …) never on
  the server; the server holds only the "MatonOS updatable apps" key and the
  F-Droid repo key, under a dedicated locked-down user, with offline backups (losing
  either key breaks updates for installed devices).
- **Gap: CPU microcode** is not loaded early. Add an early-microcode cpio
  (intel-ucode/amd-ucode from linux-firmware) as the first initrd in the
  loader entries (live + installed); old machines (Surface Pro 3) often have
  stale BIOS microcode.
- v1 builds are not secure builds (userdebug, permissive SELinux, AOSP test
  keys, no AVB). Secure releases (≈v3): `user` builds, enforcing SELinux,
  own release keys, AVB, signed shim/Secure Boot.
- **Versioning: Ubuntu-style `YY.MM.P`** (e.g. 26.12, point release
  26.12.1; internally 26.12.0). v1–v4 remain development milestones;
  releases are named by date. Set in the rename pass: `ro.matonos.version`,
  build display ID ("MatonOS 26.12" in Settings → About), image/OTA file
  names (`matonos-26.12.0-live-x86_64.img`), updater feed comparisons.
- **Release schedule: twice a year**, each release tracking one AOSP source
  drop (+ kernel/Mesa bumps and accumulated AOSP security merges). Apps
  (browser, Fossify, …) update continuously via
  F-Droid. OS-level fixes can lag up to ~6 months: state this in the README.
  Unscheduled point releases (kernel stable bump + specific fix) only for
  critical issues.

## Test devices

1. **Surface Pro 3**: Intel Haswell (HD 4200/4400/5000) → needs crocus/hasvk;
   2160x1440 12" (high DPI); Marvell 88W8897 Wi-Fi (mwifiex, only network);
   HID touch/pen/Type Cover. Secure Boot off: Vol Up + Power → UEFI;
   USB boot: Vol Down + Power.
2. **Build PC**: Ryzen 7 5800X, Radeon RX 6600 (RDNA2 → radeonsi/radv via
   gbm_mesa), Realtek r8169 Ethernet, Intel 7265 Wi-Fi.
3. Spare: han-mc-server (HP ProDesk 600 G1, i5-4690, HD 4600 Haswell).

Priorities from the test devices:
- **v1 must-have**: crocus + hasvk, Intel generation check in
  pc-gpu-detect.sh; automatic display density from EDID (physical size) in
  the pre-boot script (fixed 160 dpi is unusable on the Surface).
- **v1.x**: Wi-Fi HAL + wpa_supplicant (Surface has no Ethernet); ALSA-backed
  audio HAL (current AIDL example HAL has no real sound).
- **Wi-Fi plan (v1.x, first)**: kernel drivers + firmware already in
  (iwlwifi, ath9k–ath12k, mt76, rtw88/89, mwifiex, brcmfmac; all nl80211).
  Android: wpa_supplicant_8 with the generic nl80211 driver + supplicant
  AIDL HAL + wificond; **no vendor Wi-Fi HAL** initially (optional; only
  RTT/logging extras). Interface is `wlan0` (no udev renaming). Add
  wireless-regdb `regulatory.db(.p7s)` to /vendor/firmware. Per-device quirks
  (e.g. Surface mwifiex power save) via module options from the pre-boot
  script. Later: hostapd (hotspot), P2P. Bluetooth: AOSP stack + generic
  Linux HCI (hci0) Bluetooth HAL (as cuttlefish).
**Audio plan (user decision 2026-09-25 ~21:00)**: use BayLibre's generic
AIDL HAL from `hardware/baylibre/audio`, pinned at
`a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5`. A timeout-limited ODM oneshot
chooses the first non-Loopback/non-Dummy sound card with a playback PCM and
sets `persist.vendor.audio.primary.card` / `.device`. Static policy schema
7.0 includes `default`, `r_submix`, `usb`, and `stub`, with no Bluetooth/LE
audio. No generated policy, bind mount, ALSA loopback load, PipeWire,
WirePlumber, proxy, or custom MatonOS HAL. If there is no card or the selector
fails, init enables BayLibre's paced primary stub while HAL registration
continues. The imported APEX is named `com.android.hardware.audio.generic`,
not `com.android.hardware.audio.baylibre`; no source-tree edits are allowed.
Upstream VINTF is core v3; this AOSP checkout only provides frozen core v4,
not v5, so the ODM override declares v4 and only configured module instances.
Test HDA and no-card fresh boots, playback to a non-silent QEMU WAV, and a
selector failure. Assess SOF firmware/topology separately if needed.
- **Camera**: (1) USB/UVC cameras (webcams, most desktop/older laptop
  built-ins, likely the Surface Pro 3) via AOSP's External Camera HAL
  (V4L2 + libyuv; needs config xml, SELinux for /dev/video*, feature
  android.hardware.camera.external) → v1.x/v2. (2) MIPI + ISP cameras
  (Surface Pro 4+, most ~2021+ laptops, Intel IPU3/IPU6) need libcamera's
  Android HAL built out-of-tree like Mesa, per-sensor tuning → **v4**. App:
  Open Camera handles external cameras. Check gbm_mesa YUV (NV12) buffers.
- **Later**: sensor HAL (rotation), suspend tuning per device.

## After v1 boots (planned)

- **Stubs → real hardware**: Wi-Fi and Bluetooth managers are stand-ins
  while those services don't run (`patches/packages/modules/{Wifi,Bluetooth}`,
  `patches/frameworks/base/0003`): permanently-off managers instead of null,
  so callers like Developer options don't crash. When Wi-Fi (wpa_supplicant
  nl80211) and Bluetooth (HCI user channel HAL + detection) land, the real
  services run on hardware that has them and the stubs only remain the
  no-hardware fallback.
- **Absolute pointers (VMs), done**: QEMU/VirtualBox/VMware tablets report
  ABS_X/ABS_Y + mouse buttons without BTN_TOUCH; stock inputflinger had no
  class for them (the QEMU USB tablet became a "rotary encoder", clicks were
  dropped). `patches/frameworks/native/0001-inputflinger-support-absolute-mice`
  classifies them as cursors and CursorInputMapper sends the scaled position
  as the cursor position (PointerChoreographer's absolute-mouse path), with
  no acceleration. Verified in QEMU: pointer lands at the exact spot,
  drag-to-unlock and clicking a launcher icon work, no pointer grab needed.
- **Sleep is MatonOS's, not Android's (decided 2026-09-24).** Android is
  cut out of the sleep loop: `config_useAutoSuspend=false`, screen-off
  timeout "never" (SettingsProvider RRO), short power press ignored
  (`config_shortPressOnPowerBehavior=0`). `power/matonos-sleepd.c` (vendor
  daemon) reads every /dev/input device (never grabs), and suspends the
  kernel (`mem` → /sys/power/state, s2idle) after
  `persist.vendor.maton.sleep_idle_s` (default 900 s, 0 = never) without
  input, on a short power press (long press = Android power menu) or lid
  close; power presses within 2 s of resume are ignored (the wake key).
  Android never sees the screen go off, so there is no Doze/display off-on
  dance. Why: Android's path (Doze without a doze component, ColorFade
  screenshot failures) left the PC stuck or with a black screen on wake.
  Wake sources: `power/pc-wakeup.sh` (USB keyboards/mice + hubs/controllers,
  i8042 KBD/AUX); `patches/frameworks/native/0002` lets PS/2 mice wake the
  screen. QEMU: USB can't wake the guest (no PME/ACPI wake in qemu-xhci), so
  `run-qemu-live.sh` uses the PS/2 keyboard + vmmouse (absolute).
  **v2: Android ↔ sleepd bridge** (user's plan): a small privileged system
  service/app that (a) forwards Android's state to matonos-sleepd so it
  doesn't suspend while something needs the PC awake (Android wakelocks,
  audio playback, downloads, "keep awake" apps), and (b) receives sleep
  events from sleepd ("about to sleep", "resumed") to lock the session
  (keyguard) before suspend. Interface: a small AIDL/binder or socket API
  exposed by sleepd, guarded by a signature/privileged permission (same
  pattern as the v2 installer service). Also later: real display blanking
  before suspend (backlight / DPMS).
- **Preinstalled apps (decided, v1.1)**: F-Droid + Privileged Extension,
  Fennec F-Droid (browser), Fossify Gallery/Calendar/Contacts/Clock/Notes/
  Calculator/Music Player (replacing the AOSP ones), Open Camera.
  Not preinstalled: Fossify File Manager, QuickSearchBox, Dialer, Messaging,
  HeliBoard, Taskbar. Keyboard stays LatinIME. F-Droid's "Install unknown apps" is on
  by default (`setup/matonos-setup.sh`, only while untouched).
- **Launcher UI = Expo (React Native + Hermes, CNG, local Expo modules) (decided 2026-09-25, user)**: MatonOS Shell and MatonOS Settings are Expo projects (user is most comfortable with Expo; android/ generated by expo prebuild): all UI (shelf, home, app drawer, recents) is React Native; Java keeps only the shelf overlay window, bridge/task glue and Home handling (Hermes, New Architecture, react-native-paper MD3 + dynamic color; target UI kit = Expo UI Jetpack Compose (@expo/ui), Paper transitional — user migrates), JS bundle embedded in the Gradle-built APK. Launcher moves to Static Hermes (AOT) eventually — its code stays strictly typed TS without dynamic JS features. User edits the UI in JS.
- **Desktop = AOSP desktop windowing (decided 2026-09-24)**: Launcher3
  stays the launcher; the ChromeOS-like desktop (freeform captioned windows,
  persistent taskbar) is switched on via the framework overlay
  (`config_isDesktopModeSupported`, `config_canInternalDisplayHostDesktops`,
  dev option, `config_enterDesktopByDefaultOnFreeformDisplay`; the gating
  aconfig flags are already on in cp2a). WM Shell switches the built-in
  display between desktop-first and touch-first from the input devices
  (`enable_display_windowing_mode_switching`): stock needs keyboard +
  touchpad (laptop) or an external display; `patches/frameworks/base/0002`
  also accepts keyboard + mouse (desktop PC). A tablet without its keyboard
  stays touch-first. (Setting the display freeform from a script or a WM
  patch doesn't stick: the shell resets it.)
  Tried and dropped: Taskbar (farmerbb) as home screen (user didn't like
  it). The SystemUI patch (`patches/frameworks/base`) is kept: classic
  navigation bar if `ro.matonos.classic_navbar=true` or no recents provider
  is installed; neither is the case now.
  Everything else is user-installed. Implementation: `tools/fetch-apps.sh` downloads pinned
  F-Droid release APKs + SHA-256 check into prebuilt/apps (git-ignored);
  `android_app_import` (presigned, product partition) with `overrides` to
  drop the replaced AOSP apps. Keep DocumentsUI (system file picker). Open:
  drop Dialer/Messaging (no modem on PCs)?
  Keyboard: stays AOSP LatinIME (decided 2026-09-24). HeliBoard declares
  only a locale-less subtype until first launch, so the framework never
  enables it as the default IME on a fresh device; users can install it
  from F-Droid.
  **Requirement: F-Droid must be able to update every preinstalled app.**
  So each APK comes from the exact repo F-Droid updates from (f-droid.org
  main repo; its signer, whether F-Droid's or the developer's for
  reproducible builds), is never re-signed, is never platform-signed or
  privileged, and stays a normal updatable app. Verify in v3 that F-Droid
  lists them all as updatable.
- Browser: preinstall **Fennec F-Droid** (Firefox rebuilt from Mozilla
  source, no Firefox trademarks/telemetry; x86_64 builds; updates via F-Droid
  in v3) as a presigned prebuilt APK in `product` (not privileged, so
  F-Droid updates replace it). Official Firefox only if Mozilla's trademark/
  distribution terms allow preinstalling, and it wouldn't update without
  Play. Add after the first successful boot (v1).
- **Vulkan UI per GPU** (after a stable GL baseline): keep
  `TARGET_USES_VULKAN := false` (ro.hwui.use_vulkan) as the safe default;
  pc-gpu-detect.sh sets `debug.hwui.renderer=skiavk` +
  `debug.renderengine.backend=skiavkthreaded` at early-init only when a
  known-good Vulkan HAL was selected (radv first, then anv/hasvk), else the
  GL variants. No Vulkan: QEMU virgl (venus needs host Vulkan), nouveau (no
  NVK), softpipe fallback (no lavapipe), Haswell until hasvk.
- **QEMU Vulkan passthrough (venus)**: `run-qemu-live.sh -g venus`
  (virtio-vga-gl with blob/hostmem/venus + memfd RAM backend). Host checked:
  QEMU 11.0.2 with venus, virglrenderer 1.11 (venus strings present). Guest
  uses Mesa venus (`vulkan.virtio`). Because PCI IDs can't tell virgl from
  venus, the UI only switches to Vulkan on virtio after a boot-time probe
  confirms a Vulkan device (small probe binary, also reusable for the
  per-GPU Vulkan UI plan).
- **Wanted Vulkan drivers (user):** `hasvk` (Haswell/Ivy Bridge Intel) and
  `NVK` (NVIDIA via nouveau). NVK needs Rust in build-mesa.sh (NAK compiler:
  rustup target x86_64-linux-android + bindgen + cbindgen in the Meson cross
  build); best on Turing+ (GTX 16xx/RTX 20xx and newer); enables Zink as a
  better GL path on NVIDIA than gallium nouveau.
- Older Intel GPUs: add Mesa `crocus` (GL, Gen4–7.5) and `hasvk` (Vulkan,
  Gen7/7.5: Ivy Bridge/Haswell/Bay Trail). pc-gpu-detect.sh must then pick
  the Intel Vulkan HAL by PCI device ID (hasvk vs anv), not just vendor.
  Test box: han-mc-server (HP ProDesk 600 G1, i5-4690, HD 4600 = Haswell),
  `ssh han-mc-server` (key auth). Its 30 GB disk rules it out as a build host.
- Monorepo (user's plan): `device/maton/` becomes the single MatonOS git repo
  (product config, kernel config, graphics, patches/, apps/, services/,
  sepolicy/, tools/, manifest/), pulled into the AOSP tree by the local
  manifest so Soong builds apps/services automatically. Upstreams (AOSP,
  kernel, Mesa, firmware, NDK) stay external but version-pinned. Not yet
  under git: init + baseline commit during the rename pass. **Licence: Apache-2.0** (LICENSE). **Hosting: GitHub.** Release images
  must be compressed (2 GiB per-asset limit; split or self-host if still
  larger); no binaries in git; Actions only for cheap checks (shellcheck,
  pc.config symbols vs pinned kernel), real builds stay local. Never push
  without the user's go-ahead.
- Faster builds (host is fixed at 23 GB RAM, no hardware upgrade):
  - `build-mesa.sh`: incremental (reuse build dir; only reconfigure when
    options change) instead of wiping on every run.
  - `build-kernel.sh`: a dev mode without ThinLTO (the LTO link dominates
    kernel rebuilds); keep LTO for releases.
  - Soong incremental analysis for faster `m` startup.
  - Tune AOSP `-j` for the RAM limit (Java phases swap heavily at -j12).
  - **Trim the checkout** (biggest memory lever: soong_build peaked at
    ~18 GB and was OOM-killed once; its memory scales with the number of
    Android.bp modules): local-manifest `remove-project` / `repo init -g`
    filters for Pixel device trees (device/google/shusky, caimito, …),
    automotive/TV/Wear and SoC-vendor projects; iterate with `m nothing`
    and restore anything that turns out to be a dependency.
  - Batch config/Android.bp changes (each one triggers full Soong
    re-analysis); `m --skip-soong-tests`; `-j6` for Java-heavy phases.
  - Host: 32 GB swap file on NVMe (/home/swapfile, pri 10) added after the
    OOM kill; zram (24 GB) lives in RAM so gives less headroom than it seems.
  - Document the `m <module>` + `adb remount`/`adb sync` loop for testing
    without rebuilding images.

## Future ideas (not planned)

- Google Play services (**decided: v4, via the add-on slot**): system partitions are read-only erofs, so GApps can't
  be added by copying into /system after install. Options: build-time switch
  (e.g. WITH_GMS=true inheriting MindTheGapps x86_64), an add-on partition
  overlaid on /product (same mechanism as the protected add-on hooks), or
  microG (needs a signature-spoofing framework patch). GMS can't be shipped
  in images (licensing); uncertified-device registration and failing Play
  Integrity are unavoidable. ARM-only apps additionally need native bridge
  (libndk_translation / libhoudini).

- Android-based installer: boot the Android image live from USB (e.g. a
  live mode with userdata on tmpfs/zram) and install from an Android app,
  replacing the Linux initramfs + `installer.sh` flow of task 5.

- Linux userspace alongside Android: glibc distro rootfs in a container on
  userdata (shares the kernel and `/dev/dri`, its own Mesa), or a crosvm VM.
  Two libcs coexist per-process; no libc sharing needed. Keep namespaces,
  cgroups, overlayfs, KVM/vhost and loop enabled in `pc.config` so this stays
  possible without a kernel change.
- Protected add-on hooks (e.g. extra kernel modules such as NVIDIA's):
  - Slot: ueventd modalias loading already reads `/odm/lib/modules` as well
    as `/vendor/lib/modules`; use an `odm`/`odm_dlkm` partition, or a separate
    GPT partition the updater never touches (installer change).
  - Protection: `CONFIG_MODULE_SIG` + `MODULE_SIG_FORCE` with our own key
    (currently off in pc.config), plus own-key Secure Boot for bzImage and
    ramdisks, otherwise the kernel itself can be swapped.
  - Mainline has no stable module ABI: add-ons are rebuilt per kernel
    release (DKMS-like), so keep headers/build tree per release and use
    per-`kernelrelease` directories.
  - NVIDIA: open kernel modules would build, but NVIDIA ships no Android
    userspace; the Android graphics path is Mesa NVK (+ Zink for GLES) on
    nouveau/nova. NVIDIA's own module mainly matters for compute or the
    Linux-container idea.

## Bring-up status

All six tasks are built and run: kernel (7.2.y, Rust binder), Mesa 26.2 +
BlissOS minigbm (gbm_mesa) + drm_hwcomposer, `tools/build.sh`,
`tools/make-live.sh`, `tools/run-qemu-live.sh`.

Boot log (QEMU q35 + OVMF, USB-attached live image, virtio-vga-gl):

| Boot | Result | Fix |
|---|---|---|
| 1–3 | init: no cpuset v1, no `/dev/binder` | `CPUSETS_V1`, `ANDROID_BINDER_DEVICES` list, Rust binder |
| 4–5 | SurfaceFlinger waits for composer/allocator | init refused unlabelled vendor binaries → `file_contexts` + `tools/check-selinux-labels.sh` |
| 5 | SF abort "couldn't find an OpenGL ES implementation" | `build-kernel.sh` had wiped `prebuilt/mesa` |
| 6 | hwc: no display (card0 = simpledrm gone, virtio is card1) | drm_hwcomposer patch: scan past card gaps |
| 7 | GL renders (virgl on host GPU; in-guest screencap shows logo) but screen black: hwc `could not create drm fb -2` | kernel patch `0001-drm-virtio-allow-RGBA-scanout-with-virgl` (virtio-gpu only took XRGB/ARGB; SF always allocates RGBA_8888 for physical displays) |
| 8–10 | same watchdog: audioserver waits for every `IModule` the HAL declares, the HAL only creates those in `audio_policy_configuration.xml` | ship AOSP's generic policy (primary + r_submix); patch Bluetooth audio out of the HAL APEX |
| 11 | **`sys.boot_completed=1`**. Then: screen timeout → S3 suspend, and virtio-gpu stays black after wake; `com.android.bluetooth` aborts (no HCI HAL) | QEMU harness: `-global ICH9-LPC.disable_s3=1` (s2idle instead); vendor `unavailable-feature` for Bluetooth |
| 12 | **Home screen** (Launcher3, 1280x800, ABGR8888 scanout via drm_hwcomposer + virgl) about 1 min after power-on; no crashes, no restarting services. Known: a few `could not create drm fb -22` around the boot animation → launcher switch (hwc falls back to client composition); QMP `screendump` returns "no surface" while a GL scanout is up, so use in-guest `screencap` | — |
| 13–15 | User report "can't interact" in the QEMU window: suspended 10 s after boot with no wake source; USB tablet classified as rotary encoder | inputflinger absolute-mouse patch (usb-tablet works, no grab); VM wake = power button; `pc-wakeup.sh` for keyboard wake on real PCs (untested) |
| 7 | system_server killed by watchdog: main thread stuck in `AudioService.<init>` → `AudioSystem.isMicrophoneMuted` waiting for AudioFlinger | effects HAL exited (no `audio_effects_config.xml`) and took audioserver with it → inherit `hardware/interfaces/audio/aidl/default/audio_effects.mk` |

Debugging tips: `tools/qemu-shell.py <serial sock> '<cmd>'` runs commands in
the debug entry's root console; the console shell sits in the *bootstrap*
mount namespace, so use `nsenter -t $(pidof system_server) -m` to see the
full APEX set. Java-based tools (`input`, `svc`, `am`, `pm`) also need
system_server's environment (`BOOTCLASSPATH` etc.): prefix them with
`export $(tr '\0' '\n' < /proc/$(pidof system_server)/environ | xargs);`.
ANR traces are in `/data/anr`. If a headless VM stops answering, check QMP
`query-status` for `suspended` and send `system_wakeup`.
