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
- **v1.1 (historical store plan superseded)**: the earlier plan preinstalled
  F-Droid (+ Privileged Extension) and system-app replacements (Fennec,
  Fossify suite, Open Camera) updated from F-Droid's main repo. The target
  store/update architecture is now the
  [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md); no F-Droid client or
  GPLv3+ client code is planned for the image (F-Droid Basic was removed
  from the image 2026-10-05 under the GPLv3 rule). The current privileged app
  install path is documented under `gms/`; replacement/migration timing is
  open in the Software Centre plan. The AOSP desktop windowing mode remains.
  Our own APK repository pipeline is not a separate updater client.
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
  framework overlay points "System update" at the OS Updater. App catalogue
  and app updates are owned by the MatonOS Software Centre design in
  [`docs/SOFTWARE-CENTRE.md`](docs/SOFTWARE-CENTRE.md). No Settings
  patch. **Placement (user decision): one top-level "Hardware" entry on the
  Settings homepage (`…category.ia.homepage`)** opening our app's own page
  list: Display, Sleep, Audio devices, Wi-Fi/Bluetooth adapters, cameras,
  … (not scattered over Settings' categories). The OS Updater stays on
  Settings' "System update" entry (overlay); app updates move to Software
  Centre. Install me stays separate (live image only). Ownership:
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
  3. **OS Updater** (moved from v3): fetches and downloads OS updates from
     **han-mc-server** (static HTTPS feed) and applies them with
     `update_engine` to the inactive slot (A/B design under v3 below, now v2).
     It handles operating-system images only; app updates belong to the
     [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md).
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
  Install sequence (user, 2026-09-28): partition -> copy live slot A ->
  clone A to B -> disable the installer on the installed image. User files
  are NOT copied (user decision: installed system starts with empty /data).
  Requires the product to be A/B (slot-suffixed dynamic partitions, per-slot
  systemd-boot entries, a boot-control HAL); the live image keeps only A.
  Service scope (user, 2026-09-28; corrects an earlier note): the install
  SERVICE stays THIN by design — it only exposes base system info (disks,
  sizes, verified identity, which one is the live medium) and executes
  simple, individually validated primitive operations. ALL deciding
  (layout, sizes, step sequence, what to verify) lives in the MatonOS
  Settings app (TypeScript/Expo, createV1Plan.ts), which the user can read.
  After A/B works, trim the service back to info + primitives (anything
  that decides moves up into Settings); what remains is small enough to be
  readable — written in plain C (user preference, like matonos-sleepd.c);
  C++-only pieces (liblp for LP metadata) are reached through AOSP's tools
  (lpmake/lpadd-style) or a tiny C-callable shim, not by writing the
  service in C++. "Never the live disk" and identity checks stay in the service
  as the last line of defence. Python rejected on device (no runtime).
  Boot chain (user, 2026-09-28): systemd-boot stays; each slot's kernel is a
  signed UKI (kernel + microcode + ramdisks + cmdline with slot suffix). Slot
  switching and rollback use systemd-boot boot counting (entry renames in
  the ESP by our boot-control HAL); no EFI NVRAM writes. Firmware boots the
  fallback BOOTX64.EFI (shim -> systemd-boot with Secure Boot). Direct
  EFI-stub boot without a loader was rejected (no A/B rollback, NVRAM
  fragility, no recovery menu).
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
- **v3** (OS updates moved to v2; app catalogue and updates are now designed
  by the [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md)). OS update design,
  now built in v2: reuse AOSP `update_engine` (SELinux-confined,
  downloads signed payload.bin from our server, writes inactive slot) + a
  client app. Needs A/B: "A/B with dynamic partitions" (superseded 2026-09-29: plain Virtual A/B, see below), kernel +
  ramdisks in two XBOOTLDR partitions (boot_a/boot_b, FAT) that systemd-boot
  reads natively, systemd-boot boot counting for automatic fallback, and a
  custom boot-control HAL driving the loader entries + androidboot.slot_suffix.
  Natural point to enable AVB.
  **OS updater** (v2): system app; LineageOS's Updater (Apache-2.0: JSON feed →
  download → update_engine) is a good base. Feed + OTA files on
  **han-mc-server** as static HTTPS (decided 2026-09-24). Needs real
  release keys (not AOSP test keys), kept off the build machine.
  **System app updates between OS releases:** replaced by the
  [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md), which owns the shared
  APK, Flatpak and MatonOS APEX catalogue/update experience. The former plan
  to preinstall the F-Droid client plus Privileged Extension for silent
  installation is dropped, as is its small privileged JSON-index updater
  fallback. F-Droid-format repositories remain supported through the APK
  Source; the client app and its GPLv3+ code do not ship. The Software Centre
  design retains per-app signing keys, OS-release permission allowlists and
  the rule that app updates cannot gain newly privileged permissions. Its
  APEX channel is separate from the F-Droid repository and accepts no custom
  origins. The A/B OS Updater remains a separate feature.
  **Update 2026-09-30 (coordinator, from installexec's VAB-FEASIBILITY.md):
  plain dm-snapshot VAB is REMOVED in this AOSP** (libsnapshot
  CreateUpdateSnapshots returns an error for the legacy mode,
  system/fs/fs_mgr/libsnapshot/snapshot.cpp:3559). But AOSP ships the ublk
  route officially: vabc_features.mk sets ro.virtual_ab.ublk.enabled=true and
  snapuserd serves snapshots over mainline ublk (snapuserd/ublk_block_server.cpp;
  first-stage init: first_stage_mount_android.cpp:194). So installed systems
  use **VABC over ublk** (inherit virtual_ab_ota/vabc_features.mk, ublk on;
  compression allowed): no dm-user, no porting. Kernel: CONFIG_BLK_DEV_UBLK
  must be available in first stage (built-in or first-stage module).
  Everything else below (tight super, COW in /data, rollback) still holds.
  **Decided 2026-09-29 (user): installed systems use plain Virtual A/B**
  (Android 11 style, build/make/target/product/virtual_ab_ota/launch.mk):
  one set of partitions in super, the update is written as a dm-snapshot COW
  in /data, merged after the first successful boot; rollback until then.
  dm-snapshot is mainline (CONFIG_DM_SNAPSHOT), no dm-user/snapuserd, so no
  compression (COW = size of changed blocks). Compressed VAB later only via a
  snapuserd port from dm-user to mainline ublk (not FUSE: loop+FUSE under
  /system is slow and can deadlock). Boot counting/per-slot UKIs stay.
  Downloads stay compressed regardless: payload.bin ops are brotli/xz/zstd
  (+ deltas); VAB "compression" only shrinks the on-disk COW in /data.
  Earlier (superseded): **installed systems use A/B from v2** (super for two slots +
  boot_a/boot_b); the live image stays single-slot. One A/B build serves
  both: make-live.sh repacks only the _a partitions and passes
  androidboot.slot_suffix=_a. v1 stays non-A/B; installer.sh/make-payload.sh
  become legacy once the v2 app replaces them.
- **v3: user refines the Expo apps (Recents) personally** (user, 2026-09-27),
  once the framework (nav-bar host, no-GPU fallback) is stable; agents stay
  off apps the user claims. launchme/Shell and shelf were retired and removed
  2026-09-30.
- **v3: evaluate a shared React Native runtime** (user idea, 2026-09-27),
  decided on measured PSS + APK sizes: (a) a system shared-library APK with
  RN/Hermes/common Expo modules (saves disk/update size, not memory; needs
  exact version lockstep across apps, awkward with Expo autolinking/CNG), or
  (b) one host process with one Hermes runtime rendering Recents (and other apps)
  as surfaces embedded via SurfaceControlViewHost (saves tens of MB PSS per
  app; gives up independent updates and crash isolation).
- **Hosting (user, 2026-09-28)**: OS updates and our F-Droid repo are served
  from the user's HP server (`han-mc-server`, HWFM static file server) at
  https://download.hanro50.net.za/matonos — F-Droid repo under `fdroid/repo`,
  OS update payloads under `updates/`; driver add-ons are keyed to a
  MatonOS release and live with it under `updates/<version>/addons/`. Behind Cloudflare's edge cache: index /
  update-manifest files get `<file>.hwfm` sidecars with a short
  Cache-Control/CDN-Cache-Control (HWFM 4f86397); payloads/APKs are uploaded
  before the index that references them. Signing keys stay on the build PC.
- **"Bring your own GApps" in Settings (user, 2026-09-28)**: our stand-in for
  TWRP-flashed GApps (we have no custom recovery). The user supplies Google's
  APKs or a MindTheGapps-style zip (x86_64 only; MindTheGapps 17 is arm64-
  only so far); Settings verifies each APK's Google signing lineage and
  installs Play services / Play Store / Services Framework as updates over
  the microG stand-ins via patch 0003 (needs a GsfProxy -> Google GSF row),
  so they inherit system privileges without touching read-only partitions.
  "Uninstall updates" returns to microG. Settings also shows the device ID
  for Google's official uncertified-device registration page. We never
  ship, host, link to or auto-download Google binaries, and we never spoof
  Play certification/Integrity (no other vendors' fingerprints).
  Privileged permissions need no Google allowlist: updated system apps are
  exempt from the privapp allowlist (AppIdPermissionPolicy), so genuine
  Google apps installed over the privileged stand-ins get their privileged
  permissions automatically; our XMLs only cover what microG requests.
  GsfProxy is privileged so the real Services Framework inherits that.
  Supported user zips: flat TWRP layout (MindTheGapps), nested per-app zips
  (NikGapps), LiteGapps (files/files.tar.xz -> <abi>/<sdk>/system/...); APKs
  with native code must match x86_64. Verified 2026-09-28: LiteGapps
  x86_64/37 GmsCore, Phonesky and GSF all chain to Google's F0FD6C... root.
  Settings checks each package in order ABI (x86_64 native code only) ->
  version (must be newer than our stand-in, else Android refuses a
  downgrade) -> Google signing lineage, each with its own message. Of the
  packages tried on 2026-09-28 only LiteGapps x86_64/37 passes (MindTheGapps
  17 / NikGapps 16 are arm64; MindTheGapps 13, OpenGApps 11 and MindTheGapps
  9 x86 are older than our stand-ins, the last also 32-bit).
  The page opens with a big red warning (user, 2026-09-28; wording fixed,
  shown before anything else, must be acknowledged to continue):
  "YOU DO THIS AT YOUR OWN RISK. MatonOS makes no claim that this will work
  or be functional, and does not endorse this action. MatonOS believes in
  user choice and will not stop you from continuing at your own peril. If
  you intend to use MatonOS for a commercial use case, turn back now."
- **App stores via MatonOS Settings (user, 2026-09-28)**: Settings is NOT
  a store. It only offers to install the two stores: Aurora Store (upstream
  preload build from the MatonOS repo, installed over its placeholder via
  the pinned transitions) and F-Droid (official client from f-droid.org).
  For that it reads repo indexes itself (entry.jar signature against pinned
  fingerprints, per-APK sha256/signer) and installs silently as a privileged
  app (INSTALL_PACKAGES/DELETE_PACKAGES). It also UPDATES apps (user): with
  exactly two hard-coded repos (f-droid.org and MatonOS, fingerprints
  pinned; no repo management, no browsing) it keeps the preinstalled apps
  (Fennec, Fossify, Open Camera, ...), our apps and the stores current, next
  to MatonOS's own OS and add-on updates. F-Droid Basic is dropped from the
  image once Settings can do this.
- **App stores (user, 2026-09-28, final)**: F-Droid Basic, built from
  source by us with INSTALL_PACKAGES/DELETE_PACKAGES added to its manifest
  (small in-tree patch), signed with our key, privileged = silent installs;
  updated from our repo later. Preinstalled apps stay on their F-Droid
  builds. Neo Store removed. Aurora Store: a placeholder app (com.aurora.store,
  our key) reserves the package; the real Aurora installs over it via the
  pinned signer-transition table in patch 0003, like microG Companion → Play.
- **Audio (user, 2026-09-28, final)**: Android's audio stays as it is
  (BayLibre AIDL HAL + our ALSA card selector; no PipeWire HAL). For v4
  Linux apps, the bionic PipeWire in the Linux-apps add-on gets a sink
  (and source) that sends the mixed stream over binder to the MatonOS
  side, which plays it with AAudio/AudioTrack (records with AudioRecord
  for the source). Linux audio then enters Android's normal mix: volume,
  audio focus, media controls and Bluetooth routing all work, and Android
  stays the only owner of the hardware. Transport: one shared-memory ring
  (memfd) handed over binder once + small binder notifications, not
  per-buffer binder calls. Receiving side: the unprivileged Linux-apps host
  app (user decision; keeps audio code out of the bridge).
- **Module trust = signatures, not LoadPin (user, 2026-09-28)**: LoadPin
  (on in the distro base config) pinned all module/firmware loads to /vendor
  and would refuse driver add-ons; it is disabled in pc.config. Every add-on
  module is signed with the MatonOS module key; MODULE_SIG_FORCE is enabled
  once add-on signing is in the repo pipeline (secureboot area).
- **microG signature spoofing = LineageOS's implementation (user, 2026-09-28)**:
  patch 0002 copies LineageOS 23.2's ComputerEngine code: Google's signature is
  presented only for com.google.android.gms / com.android.vending signed with
  microG's real key and whose `fake-signature` metadata equals Google's
  certificate. No permission, no bridge policy (the bridge-based spoof
  provider and FAKE_PACKAGE_SIGNATURE were dropped). Supersedes the
  "hook that asks the bridge" design below.
- **App store transition = Neo Store, privileged; microG pulled forward (user, 2026-09-27)**:
  F-Droid 2.0 dropped Privileged Extension support and doesn't request
  INSTALL_PACKAGES, so every install prompted. Neo Store
  (com.machiav3lli.fdroid, f-droid.org repo) requests INSTALL_PACKAGES/
  DELETE_PACKAGES → shipped in priv-app with a privapp allowlist = silent
  installs with NO AOSP patch. Replaces F-Droid + Privileged Extension.
  (A trusted-installer PackageInstallerSession patch was considered and
  dropped.) Built together with microG below (agent `gms`). This is the
  current install-path decision; future shared catalogue and update UX moves
  to the [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md).
- **v3: microG (moved up from v4, user 2026-09-26)**: shipped PREINSTALLED
  for app compatibility (many apps need GMS APIs / a Play Store package) —
  a deliberate exception to v4's "sockets, not shipped binaries" (microG is
  not Google software; real Google stays user-supplied). GMS replacement via
  the system bridge — an interface to install such
  packages privileged, grant their declared permissions, register them as
  providers. **Signature spoofing (decided 2026-09-25): the ONE allowed AOSP
  patch** — a minimal (~40-line) hook in PackageManagerService that only
  asks the system bridge "may this app claim this certificate?"; all
  policy, consent, UI and revocation live in the bridge; inactive unless
  the user enables a GMS-replacement package through the bridge.
  (Taking over PMS from the bridge is impossible: PMS lives inside
  system_server and the framework calls it in-process.)
  **microG → real Google upgrade path (user, 2026-09-26):**
  - microG GmsCore + microG Companion (`com.android.vending`, the Play Store
    stand-in apps check for: presence, LVL licensing, partial billing) are
    preinstalled privileged, as one bundle.
  - ONE combined "GMS-replacement support" PMS patch (counts as the single
    budgeted exception together with the signature-spoofing hook):
    1. spoofing hook: asks the bridge whether an app may claim a cert; the
       bridge allowlist holds exact package/cert pairs (gms, vending,
       optionally gsf), no wildcards;
    2. pinned update exception: a static table `{package, installed cert
       SHA-256, incoming cert SHA-256}` lets Google's real Play Store
       (Google cert) install as an UPDATE over microG Companion (microG
       cert), and real Play Services over microG GmsCore. Everything else
       keeps the normal signature check. Hard-coded in the patch, never
       decided by the bridge (update verification is too security-critical
       for runtime policy). Pin Google's lineage if Play's key rotates.
    Spoofing must never influence update verification.
  - Result: user sideloads the real Play/GMS APK → installs as an update of
    a system app → keeps privileged status (INSTALL_PACKAGES works, no add-on
    slot needed for Play). "Uninstall updates" rolls back to microG for free.
    Remove the vending (and gms) spoof pairs while real Google is active.
  - Data dir carries over (offer "clear data" after switching). Key rotation
    can't do this without Google's private key, hence the patch.
- **Flatpak code storage and execution (decided 2026-10-04, user).** Android's
  neverallows forbid non-app domains from executing code on /data
  (system/sepolicy/private/domain.te ~1958–2051), so Flatpak code cannot run
  from a /data directory in an enforcing linuxd-created sandbox. Design:
  - Apps: one Flatpak USER installation per stub UID under
    `/data/matonos/linux/apps/<uid>/`. The signed stub records the app and
    runtime commits; linuxd verifies and publishes deployments, which Flatpak
    binds read-only into that app's sandbox. Each app's writable home is in
    the same UID-owned tree.
  - Flatpaks may download and execute code in their own app home under
    `/data/matonos/linux/apps/<uid>/` by default. The existing
    `data_exec_exempt_domain` policy patch covers this; the sandbox remains
    isolated by UID, MLS level, and its private home.
  - Runtimes and extensions: one Flatpak SYSTEM installation owned by the
    preinstalled MatonOS Linux Runtimes app. App deployments hardlink from a
    shared OSTree repo; the installer publishes updates, and sandboxes see
    their runtimes read-only.
  - Domains: linuxd → installer domain (only writer of code/runtime stores);
    flatpak run in its own narrow domain → exec of bwrap transitions to the
    bwrap setup domain → exec of the payload transitions to the app domain.
    No domain has binder; app domain gets execmem for JITs (Wine/Proton,
    Chromium).
  - Flatpak itself binds its read-only deployment and runtime into the
    sandbox. The launch chain has no mount helper; downloaded code runs from
    the app's own data under the `data_exec_exempt_domain` policy exception.
  Rejected: plain /data store (neverallow), app code inside stub APKs
  (runtime size, no namespaces in app domains), fixed partition (limits app
  count), proot (slow, weaker).
- **Split the Linux-app layer into its own repository (user, 2026-10-03;
  long-term, sooner if the provenance audit finds GPL-derived code).** Moves:
  linux/compositor (native, host APK, xwayland-egl), linux/dbus-broker, the
  stub generator, later PipeWire and the Java portals. Stays in MatonOS:
  linuxd + the Flatpak build + sepolicy + System Bridge (the "Flatpak host
  service"). Contract between them: the System Bridge AIDL and the minimum
  Flatpak version. Benefits: its own APK release cycle, contained licensing,
  reusable by other AOSP distros.
- **Addons, unscheduled (user, 2026-10-03)**, delivered through the add-on
  slot rather than the base image:
  - **ARM64 Android apps on pc_x86_64 via Digitalis** — NEXT after Flatpak
    support is done (user, 2026-10-03). Not a downloadable addon after all:
    it must live in /system, sets ro.dalvik.vm.native.bridge (+3 props),
    changes TARGET_NATIVE_BRIDGE_* in BoardConfig (Soong builds an arm64
    guest bionic), so it is a source integration in the base image (or a
    WITH_DIGITALIS build variant). It targets AOSP 16 (android16-qpr2,
    API 36); MatonOS is Android 17, so port their
    frameworks/libs/binary_translation fork. Apache-2.0; no SELinux policy
    shipped (fix avc on first boot). Research:
    out/pc-logs/agents/opencode-digitalis-result.md.
    Port 16->17 (research 2026-10-03, opencode-berberis-diff-result.md):
    MEDIUM, ~1-2 weeks for someone who knows Berberis. AOSP 17 added the
    arm64 plumbing (native_bridge, kernel_api, guest_loader, runtime,
    ld.config.arm64, binfmt) but not the engine (decoder/interpreter/JITs) —
    Digitalis's ~190k lines drop in; work = 47 conflicts (mostly Android.bp),
    cpu_emulation/ reorg moves, ~4.4k lines reapplied, API 37 trampolines.
    libnativebridge is identical 16->17; proxy list unchanged.
    Maintenance (opencode-berberis-maint-result.md): medium-high; mainly an
    annual rebase, monthly ASB = bundle rebuilds; Digitalis bus factor 1.
    Approach: integrate and help keep Digitalis alive (pin base, rebase
    yearly, interpreter + lite JIT required, heavy optimizer optional, adopt
    their test corpus), not own a translator; watch for Google publishing an
    arm64 engine upstream.
  - **Podman** (later): a linuxd-managed system Podman service (subuid
    mapping, delegated cgroup v2 subtree, pasta networking, fuse-overlayfs
    storage); Flatpak front-ends such as Podman Desktop reach it through its
    API socket. Podman inside a Flatpak sandbox is blocked by Flatpak's
    --disable-userns by design.
- **v9 (user, 2026-09-30): arm64.** Prepare now by splitting the tree:
  a shared `maton_common` (bridge, rn-apps, daemons, Linux-apps stack,
  sepolicy, overlays, install/VAB, tools) and per-architecture device
  folders (`pc_x86_64` now, `pc_arm64` in v9: kernel config, board config,
  firmware, Mesa driver set, pc-gpu-detect rules, boot path). Remove the
  hard-coded x86_64 bits (Expo abiFilters, NDK triples, QEMU harness) as
  part of the split. Scheduled right after the 2026-09-30 release.
  **UEFI is always required (user, 2026-09-30).** Hardware description:
  firmware ACPI when present (arm64 servers, Pi UEFI firmware); otherwise a
  devicetree delivered through UEFI — our signed UKIs carry the mainline DTBs
  and systemd-stub picks one by hardware ID (.dtbauto), so Secure Boot still
  covers it (DtbLoader-style UEFI driver as an alternative). ACPI-dependent
  code (sleepd wake sources, battery/lid, thermal, power button) gets a
  devicetree discovery path in the split.
  **16 KB page sizes (user, 2026-09-30): MANDATED for v9 arm64.** Android
  15+/17 expects 16 KB pages on ARM64, and most arm distros are moving that
  way, so the arm64 target will require them. x86_64 is architecturally
  fixed at 4 KB (`arch/x86/Kconfig` selects only `HAVE_PAGE_SIZE_4KB`; no
  `PAGE_SIZE_16KB` symbol exists under `arch/x86/`, kernel 7.2.7), so
  pc_x86_64 is unaffected. Arm knob: `CONFIG_ARM64_16K_PAGES=y` (choice in
  `arch/arm64/Kconfig`, default `ARM64_4K_PAGES`). The whole stack must then
  be 16 KB-clean: NDK r30 binaries and Mesa are already aligned; the AOSP
  platform/prebuilts (ramdisks, prebuilt .so) must be built 16 KB-aligned;
  and the **Flatpak glibc runtimes** (Flathub arm builds are mostly 4 KB
  today) must be 16 KB-clean or Linux apps fail to exec/map — verify/pin or
  rebuild them as part of v9.
- **v8 (user, 2026-09-29): Nix packages** as a Linux app source beside
  Flatpak/AppImage: closures under /nix/store carry their own glibc/Mesa, so
  they run under bwrap with a read-only store at /data/matonos/linux/nix
  (system-owned) mounted as /nix/store, same sockets, D-Bus broker and
  per-app stub APKs from the package's .desktop file; default permission set
  (Nix declares none, like AppImage). Rollback via Nix generations. Full
  NixOS (configuration.nix, systemd userland) is out of scope.
- **v7 (user, 2026-09-28): NVIDIA proprietary stack for Linux apps** —
  NVIDIA's OPEN GPU kernel modules (nvidia.ko/nvidia-drm.ko, MIT/GPL, Turing+)
  as a signed driver add-on built against our kernel (closed-module GPUs out:
  redistribution grey); userspace from Flathub's
  org.freedesktop.Platform.GL.nvidia-<version> extensions (NVIDIA GL/Vulkan/
  CUDA/NVENC inside each Flatpak sandbox, auto-matched to the kernel driver).
  Android itself can't use NVIDIA's glibc-only userspace, and nvidia.ko and
  nouveau can't share a GPU, so the target is HYBRID laptops: Android + our
  compositor + display on the iGPU (Mesa), Linux apps render on the NVIDIA
  dGPU, frames cross as dma-bufs (PRIME render offload; watch modifiers,
  use linear/common ones). NVIDIA-only desktops stay on nouveau + NVK.
- **v6 (user, 2026-09-28): AppImage support** on top of the v4 Flatpak
  stack: AppImages can't run natively (they mount via FUSE and expect the
  host's glibc/GL/Wayland libs; our host is bionic). Instead the store app
  imports them: extract the squashfs (--appimage-extract/unsquashfs, no
  FUSE at run time) under /data/matonos/linux/, run under bwrap with a
  Flatpak runtime as /usr and the AppImage as /app, same sockets, same
  per-app compositor stub APK from its embedded .desktop/icon, same
  uninstall sync. Sandboxed by default (safer than on desktop Linux); a
  default permission set since AppImages declare none; some AppImages
  needing host libs the runtime lacks won't work; x86_64 only.
  **Snap: rejected (user, 2026-09-28)** — snapd needs systemd (mount and
  service units), AppArmor for strict confinement (we're SELinux), system-
  wide squashfs loop mounts, and a proprietary store backend. Apps on Snap
  are almost always on Flathub or as AppImages too.
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
  2. **GMS replacement (microG)**: moved to **v3** (user, 2026-09-26), see
     there.
- **Later idea (user, 2026-09-28): "Car mode" launcher** — our own Expo UI
  launcher with an Android Auto-style layout (app rail + large dashboard
  cards: now playing via media sessions, the user's maps app, phone, apps;
  day/night, big touch targets) for PCs in cars/vans/workshops. Own design
  and icons: the real Android Auto UI is Google's closed app; AOSP's
  Automotive CarLauncher/car-ui-lib are open but need the car services.
- **v4**: Google Play via the add-on slot (stub/overlay), maybe the Linux
  sandbox, and turning it into a real little distro. Also: libcamera
  (MIPI/IPU cameras) and Aurora Store (optional, e.g. offered via the same
  add-on/first-boot choice as Play).
  **Linux apps direction (user, 2026-09-28):** an IMMUTABLE distro rootfs
  (signed image, shipped with MatonOS releases under updates/<version>/,
  A/B like our partitions) + a custom Wayland compositor bridging each Linux
  toplevel to an Android window (dma-buf import, Xwayland, input/IME,
  clipboard); apps installed as Flatpak (own runtimes incl. Mesa GL
  extension; needs bubblewrap / nested user namespaces) or AppImage (needs
  /dev/fuse). Our xdg-desktop-portal backend maps file chooser -> Android
  picker, open-URI -> intents, notifications -> Android; PipeWire <->
  AAudio bridge. Base distro: Debian stable (user: stability; mmdebstrap,
  pinned point release, security rebuilds per MatonOS release). Compositor
  host (app vs bridge) open. No libhybris (all our drivers are open Linux ones).
  No desktop environment (user): like Crostini/WSLg, each Linux app is just
  an Android window; the Linux session is only D-Bus, the compositor
  (+Xwayland), xdg-desktop-portal with our backend (file chooser, open-URI,
  notifications, Settings portal for Android dark mode/accent), PipeWire +
  WirePlumber, a Secret Service, fonts/icons/cursors, xdg-user-dirs on the
  shared folders. No DM, panel, Linux launcher or notification daemon.
  Prior art to build on (user, 2026-09-28): github.com/Xtr126/wlroots-android-bridge
  (GPL-3.0) — labwc/wlroots compositor with an AHardwareBuffer wlr_allocator
  over minigbm gralloc, per-window ASurfaceTransaction_setBuffer to
  SurfaceFlinger (zero-copy, overlays/scanout), Mesa iris tested, binder
  (NDK AIDL) link to a small Android host app; keyboard+mouse done, touch/
  stylus/gamepad/pointer-lock not. Runs its Linux side in Termux; we'd host
  it in the Debian container with glibc libbinder instead.
  Licensing (user, 2026-09-28): GPL code in our mono-repo is fine (as with
  the kernel). The compositor stays a separate GPL-3.0 program talking to
  Android over binder (aggregation); the Android host app and our other
  code keep their licenses as long as they don't link GPL code. GPLv3's
  installation-information rule: users must be able to run modified GPL-3
  parts on shipped images — kept by our Secure Boot design (own MOK key or
  SB off); never lock that down. Publish sources for GPL parts with images.
  **v4 Linux-apps plan (user, 2026-09-28), refined after reading Xtr126's code:**
  **NO GPL for the compositor (user, 2026-09-28 evening; supersedes every
  Xtr126/labwc/GPL-3.0 item below):** Xtr126's work is dropped entirely
  (labwc is GPL-2.0-only, the bridge GPL-3.0 — incompatible, and its JNI was
  TODO stubs). Our compositor = wlroots (MIT) + our own minimal core, with a
  CLEAN-ROOM reimplementation of the two architectural ideas only: an
  AHardwareBuffer/gralloc-backed wlr_allocator and per-toplevel output via
  ASurfaceTransaction to SurfaceFlinger. No code copied from Xtr126/labwc.
  As the OS (not a Termux app) we get direct hardware access: our minigbm
  gralloc, the DRM render node, our Mesa. Android does window management,
  so no WM layer (no labwc). Input: the host forwards public-API key/motion
  events to the wlr seat. Deps must be MIT/BSD-style (wlroots, wayland,
  pixman, xkbcommon, libdrm); Termux recipes only as build references.
  Deps (user, 2026-09-28): AOSP's own external/libdrm, libxkbcommon, pixman,
  wayland, wayland-protocols are forked (local manifest, branch matonos/v1.2,
  like minigbm/drm_hwcomposer) and bumped to what wlroots needs; only wlroots
  is vendored. Five more forks to rebase on AOSP upgrades.
  Buffers (user, 2026-09-28): private API accepted — the compositor gets
  AHardwareBuffer native handles via AHardwareBuffer_getNativeHandle
  (dlsym from libnativewindow at runtime; works in app processes) for
  zero-copy dma-buf sharing with wlroots; automatic fallback to a public-API
  path (render into an NDK AHardwareBuffer via EGL, one GPU copy/frame) if
  the symbol disappears. Re-check on every AOSP upgrade.
  Decorations (user, 2026-09-28; the opposite of GNOME): server-side by
  default — the compositor implements xdg-decoration and asks every app for
  server-side mode, so Android's desktop-mode caption bar (title, window
  buttons, drag) is the decoration (Qt/KDE, Electron-on-Wayland, most
  toolkits, X11 via Xwayland drop their own). CSD-only apps (GTK4/libadwaita
  header bars ignore xdg-decoration): a window that never negotiates
  xdg-decoration gets its host activity's caption made transparent
  (APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND, Android 15+) so the app's
  header bar shows through under Android's window controls. Never two
  visible title bars.
  Scaling (user, 2026-09-28): Android's density is the source of truth,
  per window (Configuration.densityDpi, updated when a window moves to
  another display). Wayland: wp_fractional_scale_v1 (exact factor) + wl_output
  integer scale (rounded up) + wp_viewporter for apps without fractional
  support; re-render on display change. X11 via per-app Xwayland: HiDPI /
  native-scaling mode (full-resolution X clients, not blurry upscales),
  plus per-instance Xft.dpi, GDK_SCALE/GDK_DPI_SCALE, QT_SCALE_FACTOR,
  XCURSOR_SIZE; Chromium-based apps (Steam) may get
  --force-device-scale-factor. Android font scale/display size flow to Linux
  apps too (Settings portal text-scaling-factor, Xft.dpi).
  Cursor (2026-09-28): Android keeps drawing the ONE system pointer (HW
  cursor plane); Linux apps only choose its shape. cursor-shape-v1 names ->
  PointerIcon.getSystemIcon() via setPointerIcon on the host activity (Android
  look); custom cursor surfaces (wl_pointer.set_cursor) ->
  PointerIcon.create(bitmap, hotspot); Xwayland cursors take the same path.
  Pointer lock (pointer-constraints + relative-pointer) ->
  requestPointerCapture() with raw relative motion forwarded; released when
  the app unlocks or loses focus. Size follows Android's pointer size
  (XCURSOR_SIZE set to match).
  Soft keyboard / IME (2026-09-28): the compositor implements
  zwp_text_input_v3; the host view's onCreateInputConnection() returns an
  InputConnection backed by the focused window's text-input state.
  enable/disable -> InputMethodManager.showSoftInput()/hideSoftInputFromWindow();
  set_content_type -> EditorInfo.inputType/imeOptions (email, number,
  password, no-autocorrect); set_surrounding_text -> getTextBeforeCursor/
  getSelectedText; set_cursor_rectangle -> CursorAnchorInfo; IME
  setComposingText -> preedit_string; commitText/deleteSurroundingText ->
  commit_string/delete_surrounding_text. Covers GTK3/4, Qt5/6,
  Chromium/Electron on Wayland. Not part of compositor v1.
  **X11 virtual keyboard layering (refined 2026-10-02, after checking the
  standalone Xwayland 24.1.13 has no built-in XIM server or text-input
  bridge):** layer 1 is the zwp_text_input_v3 + host InputConnection path
  above (prerequisite for everything, unlocks the Android IME for Wayland
  apps and provides the input state all X11 paths relay through). Layer 2
  (cheap, covers Latin text): the compositor owns one X connection per X11
  session and injects committed IME text via XTEST (Xwayland implements
  XTEST); no preedit, no CJK. Layer 3 (full IME): a minimal clean-room XIM
  server as a per-session X client of the compositor relaying
  preedit/commit to the same InputConnection, or vendored IBus with
  XMODIFIERS=@im=ibus and GTK_IM_MODULE/QT_IM_MODULE=ibus in the X11 env;
  apps opt in through the launch environment either way. The staged
  Xwayland 24.1.13 build excludes GLX/glamor/DRI3 and has no IME features
  of its own, so the compositor supplies the entire bridge.
- **Flatpak display selection: offer both, override via the bridge
  (user, 2026-10-02):** every graphical launch exports WAYLAND_DISPLAY and a
  per-session DISPLAY (the Xwayland socket path), grants both sockets, and
  forces no toolkit backends — the Chrome `--ozone-platform=wayland` special
  case goes away, because with a real X display present Chromium's X11
  default works. Toolkits self-select (GTK/SDL2 -> Wayland, Qt and
  Chromium/Electron -> X11). Per-app overrides run through the bridge's
  generic daemon channel, no AIDL changes: linuxd call commands
  `set_display_mode {ref, x11|wayland|both}` and `get_display_modes`,
  persisted in a bounded `/data/matonos/linux/config/display-modes.json`.
  Launch consults the override: wayland -> strip DISPLAY and restore the
  forcing vars, x11 -> strip WAYLAND_DISPLAY and set the X11 toolkit env,
  both -> offer both unchanged. **Implemented (removed the transitional
  `/data/matonos/linux/config/x11-apps` opt-in file; the forcing variables
  and the Chrome `--ozone-platform=wayland` special case are gone).** The
  bridge `set_display_mode` override commands are still future work.
  Authorization rides the bridge's existing
  target allowlist; a store/Settings toggle can come later. Telemetry comes
  free: the compositor logs which display system each session actually used
  (Xwayland `events.ready` fires only on the first X client connect), and a
  bridge topic can expose it to settings later.
  - **No Debian base (user decision, 2026-09-28, supersedes the Debian
    image):** we need the Android<->Linux glue anyway, so the host side is
    only flatpak + bubblewrap + ostree (+ their deps) built for Android
    (NDK / termux-packages recipes), and Flatpak apps run on the Android
    kernel with their runtime's own /usr. Session pieces (D-Bus, PipeWire,
    xdg-desktop-portal with our backend, flatpak-session-helper) are our
    glue, run as Android-side natives (user, 2026-09-28): D-Bus (session
    bus daemon) built for bionic; PipeWire + pipewire-pulse built for bionic
    (our earlier NDK PipeWire work was replaced by the BayLibre audio HAL —
    revive it for v4, plus an AAudio sink/source node of ours; it may also
    help with the remaining audio issues); xdg-desktop-portal + our backend
    is the glue we write anyway. Flatpak apps only see their sockets
    (bus, pipewire-0, pulse/native, Wayland), so bionic servers work with
    the runtimes' glibc clients.
    The bionic Flatpak stack (flatpak/libflatpak, ostree, glib/gio, curl +
    one TLS lib, libarchive, libxml2, zstd/brotli, gpgme+gpg, bubblewrap,
    libseccomp, json-glib, fuse3, libc++) is PRESHIPPED in the image (user,
    2026-09-28): estimated ~15-22 MB stripped (from Debian trixie sizes,
    not a build), ~6-9 MB erofs-compressed. Runtimes/apps still go to /data
    only when the user installs them. (Earlier idea of shipping it as an
    optional add-on is superseded.)
    Built with Soong on the system side (system_ext) — user decision
    2026-09-28, an explicit exception to "natives outside Soong" for this
    stack — so it links Android's BoringSSL (libcrypto/libssl), curl,
    libxml2, zstd, brotli, zlib, libfuse and libc++ instead of bundling
    them (~8-12 MB added instead of ~22 MB; TLS fixes arrive with Android).
    New third-party modules under linux/third_party/: glib, json-glib,
    libarchive, libseccomp, bubblewrap, ostree, flatpak, gpgme stack.
    Maintained as PATCHES over pristine pinned upstream sources (user,
    2026-09-28): linux/third_party/<name>/upstream + patches/NNNN-*.patch.
    Feasibility spike started 2026-09-28 (agent flatpak-spike).
    Stub APK generation + signing lives in a SEPARATE app, not the bridge
    (user: code separation); that generator app grows into the Flathub/
    Flatpak store app (the Expo "Flathub app" above) — one app for browsing,
    requesting installs via the bridge, and generating/signing stubs; the bridge owns Flatpak
    install/uninstall via matonos-linuxd (system_ext service wrapping the
    flatpak CLI). Compositor library v1 runs apps "like Termux" (single
    host app, input forwarded straight to the compositor) before per-app
    stubs exist.
    Isolation rules (user, 2026-09-28): Flatpak apps only ever link their
    runtime's glibc/Mesa inside the bwrap sandbox; what crosses is kernel
    syscalls/ioctls and device nodes, sockets (Wayland, PipeWire/Pulse,
    D-Bus), fds (dma-buf, memfd) and read-only data under /run/host. NO
    host command execution: build flatpak-session-helper without (or with
    disabled) host-command spawning and never expose org.freedesktop.Flatpak
    to apps, so `flatpak-spawn --host` can't reach the bionic system side.
    The D-Bus session bus is heavily locked down: default-deny policy,
    only our portal backend names and explicitly allowed services are
    reachable; Flatpak's own bus proxy (xdg-dbus-proxy) filters per app.
    D-Bus + UIDs (user, 2026-09-29): each Linux app runs as its STUB's Android
    UID (per-app; makes forwarding to that app's stub simple). NO
    dbus-daemon (user, 2026-09-29): each stub runs its OWN MINI-BROKER built
    on GDBus (already shipped in libgio) — single-app mode: it implements the
    org.freedesktop.DBus methods apps actually use (Hello, RequestName/
    ReleaseName, AddMatch/RemoveMatch, NameOwnerChanged, GetNameOwner,
    ListNames...) and routes between the app's own processes and the stub's
    in-process portal objects. The broker IS the policy point (default-deny;
    replaces xdg-dbus-proxy). New code: test against GTK/Qt/Electron apps;
    add bus features as real apps need them. Superseded below: dbus-daemon.
    Bus socket in the owning user's runtime dir; "only the
    bus owner's UID may connect" does the isolation; the config uses NUMERIC
    UIDs only (bionic getpwnam() knows Android IDs, not Linux users — no fake
    passwd db); default-deny policy with only our portal backend + Flatpak
    helpers allowed. With per-app UIDs the bus is per app (one dbus-daemon
    per running stub, like the per-app compositor); an app's portal calls
    reach its own stub directly. Built after `flatpak --version` works.
    Intents -> Linux apps (user, 2026-09-29; enabled by the per-stub broker):
    the stub declares intent filters from the app's .desktop MimeType= (and
    x-scheme-handler/*); an ACTION_VIEW/SEND to the stub becomes a D-Bus call
    on the app's own bus — org.freedesktop.Application.Open(uris) /
    Activate / ActivateAction for D-Bus-activatable apps, else a
    `flatpak run --file-forwarding <id> @@ <uri> @@` launch with a
    document-portal path for content:// URIs. So Android's share sheet and
    "open with" list Linux apps like any Android app.
    D-Bus service mapping in the per-stub broker (user, 2026-09-29). Only the
    names below are served; everything else stays default-deny:
    - org.freedesktop.Notifications -> Android notifications posted by the
      stub (actions -> ActionInvoked, dismiss -> NotificationClosed).
    - org.freedesktop.secrets -> per-app secret store: blobs in the stub's
      private data, encrypted with a key held in Android Keystore. (gpg is a
      signing/encryption tool, not a secret service; coordinator suggestion.)
    - org.freedesktop.FileManager1 (ShowItems/ShowFolders) -> intent to show
      the file in the Android file manager.
    - org.freedesktop.UPower -> fed from Android's battery/power APIs
      (BatteryManager, PowerManager), read-only.
    - org.freedesktop.portal.FileChooser -> Android file picker (SAF).
    - org.freedesktop.portal.OpenURI -> ACTION_VIEW, i.e. the default browser
      for http(s) and the default handler for other schemes.
    - org.freedesktop.portal.Documents -> DECIDED (user): copy-in/write-back. Hands picked files
      to the sandbox. Proposed v1: the stub copies the picked content:// file
      into a per-app doc dir bound into the sandbox and writes it back on
      save; later a FUSE view (Android has /dev/fuse) instead of copies.
    - org.mpris.MediaPlayer2 -> Android MediaSession (media notification,
      lock-screen and hardware media keys). Per-app volume is separate: it
      comes from the app's PipeWire stream going through the host app.
    - org.freedesktop.portal.GlobalShortcuts -> only if the stub holds
      "display over other apps" (SYSTEM_ALERT_WINDOW, user-granted in
      Settings); without it the portal reports no shortcuts (user).
    Further mappings (coordinator list, 2026-09-29; v1 = marked *):
    - portal.Settings* -> night mode, Material You accent, fonts.
    - portal.Inhibit* / org.freedesktop.ScreenSaver* -> wake lock via bridge.
    - portal.NetworkMonitor* -> ConnectivityManager (online/metered).
    - org.a11y.Bus* -> stub bus or NO_AT_BRIDGE=1 so GTK doesn't wait.
    - portal.Notification -> same backend as org.freedesktop.Notifications.
    - portal.Email -> ACTION_SENDTO mailto:. portal.Print -> PrintManager.
    - portal.Screenshot/ScreenCast -> MediaProjection (per-use consent).
    - portal.Camera -> camera runtime permission; frames via PipeWire.
    - portal.Location -> location runtime permission.
    - portal.Background -> foreground service on the stub.
    - portal.Trash -> MediaStore trash. portal.Account -> name/picture.
    - org.freedesktop.login1 -> read-only PrepareForSleep signals only.
    Stubs are config files; the host is an updatable library (user,
    2026-09-29). org.matonos.compositor (preinstalled system app, updatable
    from the MatonOS repo like WebView) declares a DYNAMIC shared library
    (<library android:name="org.matonos.linuxhost"/>; only system apps may,
    and updated system apps keep it). A stub APK is only: manifest (label,
    per-app UID, <uses-library org.matonos.linuxhost>, activity/service
    entries whose classes live in the host library, intent filters from the
    .desktop file, permissions from the Flatpak metadata, <meta-data> with
    the Flatpak ref, API version) + icon (+ an empty dex if the platform
    insists). No code of its own, so host fixes reach every app without
    regenerating/re-signing stubs. NOT static shared libraries (they pin an
    exact version). Host exposes an interface version; stubs declare a
    minimum and get a clear error if unsupported. Update channels: Flatpak/
    bwrap/ostree/gpg in system_ext = OS updates only; bridge = OS updates;
    host = repo + OS; stubs regenerated by the store only when the stub
    format version changes.
    Stub signing (user, 2026-09-29): on first boot a per-device stub signing
    key is generated and stored; all stubs are signed with it on-device.
    Coordinator notes: generate it in Android Keystore (non-exportable, EC
    P-256 or RSA-2048; apksig works with a Keystore-backed PrivateKey via
    JCA Signature), owned by the component that mints stubs (store app or
    bridge). Consequences: the key is per device, so stubs are
    not portable - after a restore/new device, stubs are re-minted, not
    copied (Flatpak data restored separately). Lose the key (factory
    reset) = stubs re-minted too. Only packages signed with this key and
    declaring the Flatpak <meta-data> are treated as stubs by the bridge.
    Cross-app D-Bus (user, 2026-09-29): buses stay per app; apps reach each
    other only via names declared in their Flatpak metadata. --own-name ->
    the stub registers the name with the bridge; --talk-name call -> the
    broker asks the bridge which stub owns it and relays stub-to-stub over
    binder, checking both UIDs. Talk-names are shown at install like
    permissions; wildcards (org.freedesktop.*, org.kde.*) and system-service
    names are refused; org.freedesktop.Flatpak is never reachable. In effect
    the brokers + bridge are a D-Bus <-> binder bridge.
    System services (user, 2026-09-29):
    - Avahi / NetworkManager -> limited subset, only after the app asks for
      the matching Android permission (Avahi -> NsdManager mDNS browse/
      publish; NetworkManager -> read-only state, wifi scan with location).
    - GeoClue -> Android location; calling it triggers the location
      permission prompt.
    - BlueZ -> not served (Android has no BlueZ); an Android Bluetooth API
      bridge can be exposed later if developers want it.
    - PackageKit -> minimal read-only stub: reports an atomic/immutable
      distro (no system packages to install); any install/remove call,
      including Flatpak refs, returns a permission-denied error. Apps install
      through the MatonOS store, never via the bus (user).
    - systemd -> behave like a non-systemd distro (Alpine/Void/Devuan):
      org.freedesktop.systemd1 is simply not on the bus (ServiceUnknown), so
      apps take their existing non-systemd fallbacks; org.freedesktop.login1
      is the elogind-compatible subset only (PrepareForSleep etc.) (user).
  - Prototype order (de-risking, scariest first): (1) a CLI Flatpak under
    bwrap on MatonOS (namespaces/seccomp/SELinux); (2) one Wayland window
    (weston-terminal from a Flatpak, its own Mesa) through the Xtr126 path on
    real Intel hardware; (3) a GTK4 app with menus/dialogs; (4) one X11 app
    via per-app Xwayland; (5) FINAL MILESTONE: the Steam Flatpak
    (com.valvesoftware.Steam) launches a Proton game in a gamescope "game
    mode" stub. Steam specifics: kernel 32-bit support is already on
    (IA32_EMULATION=y) and Flatpak brings the i386 runtime + GL32 Mesa;
    pressure-vessel's nested containers need our portal glue to implement
    org.freedesktop.portal.Flatpak's SANDBOXED Spawn (not host spawn) and
    nested user namespaces; Steam Input needs /dev/hidraw* and /dev/uinput
    passed into the sandbox (the steam-devices udev-rule equivalent).
    Anti-cheat works only where developers enabled Linux (as on Steam Deck).
    Known risks: Flatpak/bwrap vs Android SELinux+seccomp; dma-buf modifiers
    between runtime Mesa and minigbm beyond Intel iris; Wayland popups/CSD vs
    Android windows; input fidelity; per-app stubs as untrusted_app owning
    sockets; keeping wlroots/Xwayland/PipeWire current. v4 is months of work. This depends on
    add-on packages being able to ship userspace daemons (deferred in the
    first add-on release; now needed for v4). Debian (mmdebstrap) stays the
    fallback if this proves unworkable: measured 2026-09-28 at 321 MiB
    installed for minbase + flatpak/dbus/portal/PipeWire, ~60-100 MB as
    trimmed erofs.
  - Components (user): (1) the Linux-apps add-on above; (2) a small **Flathub
    app** built with Expo (our React rules) that browses/searches Flathub
    and asks to install/uninstall — UI only; (3) the **system bridge owns
    installing and uninstalling Flathub packages**: it drives flatpak in the
    container (via the container daemon's channel), generates, signs and
    silently installs/removes the stub APKs, and does the uninstall sync
    below. The Flathub app reaches it through a `flatpak` bridge target
    (built-in app = default-allow; third-party callers need consent).
  - Storage (user): Flatpak installs live in a hidden folder under
    userdata, not in shared storage — e.g. /data/matonos/linux/flatpak
    (system-wide installation, own SELinux label, invisible to Files/MTP,
    excluded from Android backups). Per-app DATA (~/.var/app/<id>) lives
    in the app's STUB's own Android data folder (user, 2026-09-28), bind-
    mounted into the sandbox: uninstalling the stub deletes it, Settings ->
    Apps shows its size, "Clear storage" resets the Linux app, and Android's
    per-user separation/encryption apply. Installed FILES (the app's /app,
    runtimes, the ostree repo) stay in the shared hidden store: runtimes are
    shared and deduplicated, Android neverallows system components executing
    code from app data folders, and a stub must not be able to modify its
    app's binaries; the uninstall sync removes them instead. The Debian
    base image stays read-only; its writable bits (/etc overrides, /var)
    also live under /data/matonos/linux (no base image with option C; the
    add-on's writable state lives there instead).
  - Fork wlroots-android-bridge. Its compositor (labwc/wlroots) runs as an
    ANDROID (bionic) process: launched via app_process, hands 3 binders
    (main/surface/input) to the host app inside an intent Bundle (no
    servicemanager); AIDL: registerXdgTopLevelCallback, add/removeXdgTopLevel
    (one activity per toplevel), onSurfaceCreated/Changed/Destroyed (Surface
    back to the compositor, ASurfaceTransaction per window), InputQueue ->
    wlr seat. Linux apps are plain Wayland clients: only the Wayland socket
    (and PipeWire/D-Bus sockets) crosses into the Debian container, buffers
    as dma-buf fds. The glibc libbinder is then NOT needed for display.
  - Host it in a normal, UNPRIVILEGED MatonOS app: Android's app sandbox (own
    uid + untrusted app SELinux domain) contains a compositor exploit to that
    app. Never run it as a system/privileged component.
  - Build its native deps (wayland, wlroots, labwc, pixman, xkbcommon, libdrm)
    from termux-packages recipes with our own package name/prefix instead of
    maintaining our own bionic ports; we pin and rebuild. Our minigbm fork
    is the gralloc its dma-buf import trick relies on.
  - Flatpaks as real Android apps, WebAPK-style: on Flatpak install the host
    generates a stub APK per app (package org.matonos.linux.<flatpak-id>,
    label/icon from the .desktop file, one launcher activity that asks the
    compositor to `flatpak run <id>`), signs it with an on-device key
    (apksig) and installs it silently (the bridge does this); uninstalling the Flatpak removes the stub. Windows use
    setTaskDescription(label, icon) so Recents/taskbar group them under the
    app. Same mechanism could give browser PWAs real app entries.
    Stub install flow: (1) store app -> bridge flatpak.install -> linuxd
    runs flatpak install and reports the app's .desktop + icon; (2) the
    STORE app instantiates a template stub (package org.matonos.linux.<id>,
    label/icon, <uses-library> compositor lib, one launcher activity,
    versionCode bumped on name/icon changes) and signs it with a per-device
    stub key held in Android Keystore (non-exportable); (3) it hands the APK
    fd to the BRIDGE (flatpak.install_stub), which verifies package prefix,
    pinned stub cert, manifest == template (one activity, allowed
    uses-library, only template-approved permissions) and that the Flatpak
    is installed, then installs silently via PackageInstaller. The store app
    needs no install privileges. Permissions (user question): dangerous
    permissions are RUNTIME — the stub declares only what the Flatpak's
    manifest needs (mic/camera/location/notifications) and requests them at
    first use, triggered by our glue (camera/location portals, PipeWire
    capture); denial returns normal "access denied" to the Linux app.
    Uninstall sync: the stub gets no callback, but the bridge receives
    ACTION_PACKAGE_FULLY_REMOVED (manifest receivers allowed; not sent for
    updates, unlike PACKAGE_REMOVED) for org.matonos.linux.* and runs
    `flatpak uninstall --delete-data <id>` (user decision: data goes with
    the app, as on Android) plus `flatpak uninstall --unused` for runtimes. Reverse: a Flatpak removed on
    the Linux side (its .desktop export disappears) -> host uninstalls the
    stub silently (bridge).
    Closing (user): closing a Linux app's Android window (close button,
    swiping its task away in Recents) closes the Flatpak app: the host sends
    the toplevel a Wayland close request (xdg_toplevel.close, so unsaved-work
    dialogs still work); when the app's last window is gone and the process
    hasn't exited after a short grace period, the host runs `flatpak kill
    <id>`. Force-stopping the host app kills the compositor, so every
    Wayland client loses its connection and exits too.
    EXCEPTION (user, 2026-09-28): apps with a registered tray item (Steam,
    Discord, ...) hide to the tray on window close, so they are NOT killed
    when the last window closes; their stub keeps a foreground service tied
    to the tray notification. Tray "Quit" or swiping away in Recents ends them.
    Tray: Android has none -> StatusNotifierItem (our D-Bus glue implements
    StatusNotifierWatcher) becomes an ONGOING notification posted by the
    app's stub (tray icon/title; tap = Activate; first dbusmenu entries as
    notification actions; "More" opens a small popup with the full menu).
    Legacy XEmbed tray icons go through an xembedsniproxy-style converter.
  - **Per-app compositors (user, 2026-09-28; replaces one shared
    compositor):** every Linux app's stub APK runs its OWN compositor
    instance (the Xtr126 fork, instantiated per stub), so each Linux app is
    an Android app in every respect: isolation (a compositor crash/exploit
    hits one app), lifecycle (close/swipe/force-stop ends that app only),
    per-app volume (the stub's own AAudio stream), per-app mic/camera
    permissions, Recents/taskbar/notifications grouping. The compositor and
    Xwayland code live in ONE shared library APK (system, declared
    <library>; stubs <uses-library> it, since stubs are signed on-device).
    Xwayland starts lazily per app only for X11 clients (Flatpak apps get
    the host's X socket; runtimes ship only X client libs) — per-app X
    servers also stop X11 apps snooping on each other. Xwayland ships as an
    executable in the library's nativeLibraryDir. Cross-app features map to
    Android APIs instead of a big session service: clipboard <->
    ClipboardManager; drag and drop <-> startDragAndDrop(DRAG_FLAG_GLOBAL);
    xdg-open/launching <-> intents (other Linux apps start via their stub);
    text-input-v3 <-> the stub activity's InputConnection; notifications <->
    NotificationManager; file chooser <-> SAF; screen capture <->
    MediaProjection; focus/activation <-> Android tasks. Still shared: the
    Flatpak/container side (bridge + container daemon), the D-Bus session
    bus, PipeWire. Cost: ~20-40 MB per running app (+Xwayland if X11).
  - Xtr126's code is NOT shipped as its app (user, 2026-09-28): it is the
    BASE of our system library ("run Wayland apps"): its wlroots/labwc
    compositor core, AHardwareBuffer allocator, per-window
    ASurfaceTransaction output and input path get restructured into the
    shared library APK the stubs load (one compositor instance per stub),
    with Xwayland alongside; its demo app / Termux launcher are dropped.
  - Evaluated, not chosen as the base: gamescope (Valve, BSD-2) — one-app
    micro-compositor, but composites everything into ONE output (desktop
    apps need one Android window per toplevel), X11-centric, would need a
    new Vulkan-to-ANativeWindow backend and lose the zero-copy SurfaceControl
    path; keep as a later per-app "game mode" compositor (e.g. Steam
    Flatpak). exo (Chrome OS) — Chromium/Aura-bound, not extractable;
    Sommelier (Crostini, BSD) — an in-VM proxy that still needs a host
    compositor; useful reference for X11 quirks, DPI scaling, clipboard.
  - Alternative kept on file: compositor inside the container + a small
    Android-side presenter over binder (keeps untrusted Wayland parsing in
    the container; more parts). Estimate for the fork route: ~1-2 weeks of
    agent work to reach parity (GPU-accelerated resizable windows, kbd+mouse);
    touch/stylus/gamepad/pointer-lock/clipboard/IME/portals extra.
  Linux -> Android IPC = binder (user): fds cross for free (dma-buf buffers,
  PipeWire memory, files). A dedicated binderfs device for the container
  (e.g. `linuxbinder`) whose context manager is our bridge daemon, so Linux
  apps reach only the services we publish, never Android's servicemanager.
  Container side in Rust with rsbinder (pure-Rust binder + AIDL, no bionic);
  stable, versioned AIDL interfaces (compositor/surfaces/input/IME, audio,
  portals, clipboard, shortcuts); one SELinux binder_call rule per service.
  Binder is a kernel driver (ioctl on the device), so glibc code can use it
  directly: Rust components via rsbinder; C/C++ via AOSP's own libbinder
  built for glibc (its linux_glibc host variant + libutils/libcutils/liblog/
  libbase), shipped in the rootfs and, if third-party Linux apps should call
  Only OUR components talk binder (user): build the glibc libbinder into the
  rootfs when v4 starts; no Flatpak binder extension, third-party Linux apps
  go through portals. Alternative kept in mind: libgbinder (Waydroid, Debian).
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

- **QS brightness + media volume sliders (2026-10-03):** MatonOS cp2a
  release values enable `scene_container`, `dual_shade`,
  `qs_tile_detailed_view`, `expanded_audio_detailed_view` and
  `desktop_sizing` as read-only build-time flags. Stock SystemUI's dual
  shade creates an always-present STREAM_MUSIC slider, independent of media
  playback. The SystemUI RRO retains the desktop status bar, skips touch
  falsing on PCs, and keeps QS visible during brightness adjustments.
  Keep the r23 Media volume TileService and both default tile lists as a
  panel shortcut/fallback. No AOSP patches. Host aconfig generation is
  verified; rebuilt SystemUI/RRO runtime validation is still required.
  Brightness remains upstream-limited to displays reported as INTERNAL.

- **Bluetooth/WiFi: HAL-owned stable interface, usermode spoof dropped (user,
  2026-09-30)**: the `matonos-btd` daemon and its `/dev/vhci` "empty radio"
  emulator are removed. With no controller the Bluetooth HAL reports
  unavailable and Bluetooth stays off — honest, no crash; the
  "toggle/scans-work-with-no-radio" goal is given up. We ship AOSP's **stock**
  `android.hardware.bluetooth-service.default` HAL
  (`hardware/interfaces/bluetooth/aidl/default`): it already unblocks rfkill via
  MGMT, finds the controller (`READ_INDEX_LIST`) and binds the HCI user
  channel, and reports `UNABLE_TO_OPEN_INTERFACE` when there is none. Our
  custom `matonos-bluetooth-service` HAL, `matonos-btd` and the emulator are
  deleted; the controller-choice/list API is dropped (the stock HAL uses
  `hci0`). Rationale: our custom HAL duplicated AOSP and the hand-rolled
  emulator aborted the stack (`hci_layer.cc:493`). WiFi
  likewise drops `matonos-wifid`'s rename/rfkill/2 s-poll machinery:
  `mac80211_hwsim` (module param) covers the no-radio case and the stock
  `wpa_supplicant`/`wificond`/nl80211 path owns `wlan0`. See
  `out/pc-logs/agents/BT-WIFI-SIMPLIFY.md`.

- **PipeWire moves to system_ext (user, 2026-09-30)**: it was placed in
  /vendor when it was going to be the audio HAL; Android audio now uses
  BayLibre's HAL, and PipeWire's job is Linux-app audio (v4): Flatpak apps
  talk to it over sockets and it outputs through AAudio (public NDK API,
  system side). So it is a platform-side service next to linuxd and the
  compositor host, built by the same after-Soong script approach as the
  Flatpak stack and sharing its GLib/D-Bus libraries instead of bundling
  them. Mesa stays vendor-side (Android only loads GPU drivers from
  /vendor or /odm through the sphal namespace); it may later move into the
  ODM bundle and link our vendor libdrm fork.

- **Release images (user, 2026-09-30)**: releases ship the GPT live disk as
  `.img.xz` (works written to USB and as a VM disk), published as GitHub
  pre-releases on MatonOS-dev/MatonOS (fallback: han-mc-server). Future: a
  hybrid ISO that boots when written to USB or attached as a disk (no CD
  boot). Real optical/CD boot is not planned: Android's first-stage init
  resets /dev and loses a loop-mounted super, and fixing that would mean an
  AOSP init fork (tools/README.md records the blocker).
- **Flatpak stack build = upstream build systems after Soong (user,
  2026-09-30; replaces the planned Soong port)**: tools/build-flatpak.sh runs
  each project's own configure/meson with the NDK, like build-mesa.sh, but
  AFTER the AOSP build, linking the platform's own libcurl + BoringSSL
  (libcrypto/libssl) from out/.../system/lib64 with headers from
  external/curl and external/boringssl. The result ships in system_ext as
  prebuilts; flatpak is a native system_ext binary, so it may load those
  /system libraries. Rule: the bundle is rebuilt from the same tree on every
  image build (build.sh runs it after AOSP), because platform libraries have
  no stable ABI; OTAs ship system + system_ext together. Wins: no 22
  hand-written Android.bp files (upgrades = fork bump), no bundled curl/TLS,
  BoringSSL fixes arrive with Android. Today's release ships the tested
  self-contained NDK bundle (~26 MB) as a stopgap.

- **Forks, not patch series (user, 2026-09-29, long-term goal)**: every
  change to someone else's code lives in a real repository fork in the
  MatonOS-dev GitHub org (branch `matonos/v1.2`), pulled in by
  `manifest/maton.xml`; patch series in this tree are transitional. Done:
  minigbm, drm_hwcomposer, BayLibre audio, libdrm, libxkbcommon, pixman,
  wayland, wayland-protocols (FORKS.md). Next: the 9 patched Linux-apps
  components (flatpak, ostree, bubblewrap, glib, gnupg, npth, libgpg-error,
  libfyaml, appstream), with the 13 unpatched ones fetched from upstream git at
  pinned tags; sources check out as nested projects at
  linux/third_party/<name>/upstream so our Android.bp files stay put. Later:
  the AOSP patches under patches/ (frameworks/base 0001-0003, …) become forks
  of those AOSP projects. True GitHub forks where upstream is on GitHub,
  plain repos with full history otherwise. Our own code stays in the
  MatonOS mono-repo; forks are only for other people's code.

| Area | Decision | Why |
|---|---|---|
| Lunch | `pc_x86_64-aosp_current-userdebug` | `aosp_current` alias exists in `build/release/release_config_map.textproto` |
| ABI | 64-bit only (`core_64_bit_only.mk`) | Avoid building Mesa/HALs twice; same as cuttlefish `x86_64_only` |
| Arch variant | **x86_64-v2** (SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT, ~2009+). Out-of-tree builds add `-march=x86-64-v2` (Mesa, NDK daemons, PipeWire/Flatpak, app JNI); AOSP uses an x86_64 `TARGET_ARCH_VARIANT` (`sandybridge`→`-march=corei7` gives v2 semantics; Soong has alderlake/skylake/haswell/… too). Kernel too: `KCFLAGS=-march=x86-64-v2` / `KRUSTFLAGS=-Ctarget-cpu=x86-64-v2` in `tools/build-kernel.sh` (mainline 7.2 has no 64-bit µarch choice; `CONFIG_X86_NATIVE_CPU` stays off). | Better codegen (Mesa/llvmpipe especially); all target/test PCs (Haswell 2013/14, Ryzen) are v2+. Deliberate narrowing from baseline: drops pre-2008 Core 2/K8 and restricted-CPUID VMs; the QEMU harness uses `-cpu max`/`host`, so unaffected. |
| Hardware description | ACPI + PCI enumeration, **no DTB** | x86 firmware provides ACPI; nothing like `dtb.img` is needed |
| Kernel | `TARGET_NO_KERNEL := true` | Built out-of-tree by `build-kernel.sh`, packaged by `make-payload.sh` |
| Ramdisks | `BOARD_BOOT_HEADER_VERSION := 4` | Only way to get `vendor_ramdisk.img` built; `vendor_boot.img` is produced but not shipped |
| Kernel cmdline | Lives in `make-payload.sh` → `payload.conf` → systemd-boot entry | `BOARD_KERNEL_CMDLINE`/`BOARD_BOOTCONFIG` only go into `vendor_boot.img`, which we don't use |
| Partitions | GPT `esp, misc, metadata, super, userdata`; no boot/recovery/vbmeta | Contract of `installer.sh` |
| super | 8 GiB, one group `pc_dynamic_partitions` (super − 4 MiB): system, system_ext, product, vendor | `BOARD_SUPER_PARTITION_SIZE` == `SUPER_SIZE_BYTES` (8589934592) |
| RO fs | erofs | Smaller super; erofs is in mainline |
| AVB | off | No verification in the boot chain |
| SELinux | default enforcing for live and installed entries; explicit `MATON_SELINUX_PERMISSIVE=1` development live/payload profile | Debug means logging/serial console; release packaging (`-R` / `MATON_RELEASE=1`) refuses permissive. See tools/README.md and sepolicy/r24-enforcing-test-plan.md |
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
      libclang-cpp21-dev libclc-21-dev libpolly-21-dev; SPIRV-LLVM-Translator 21 is not in apt.llvm.org:
      build llvm_release_210 from source, set MATON_SPIRV_PREFIX in matonos.local.env.
      Meson >= 1.4 (distro may ship older): release tarball + wrapper on PATH
      via matonos.local.env.

## Base: AOSP (decided)

LineageOS was considered (security merges, Updater, signing tooling) and
rejected in favour of staying on plain AOSP. Its A/B updater pattern remains
relevant to OS image updates. App updates use the
[MatonOS Software Centre](docs/SOFTWARE-CENTRE.md), not a reused F-Droid
client or Privileged Extension.

Re-evaluated 2026-09-28 (user): LineageOS 24 (android-17.0.0_r1) now has an
actively maintained generic PC target (device/pc/basic_x86_64_pc +
virt-common: Mesa, minigbm, drm_hwcomposer) and monthly security merges; it
would drop patch 0002 and maybe our graphics forks, at the cost of a
re-integration and a monthly sync/build/sign/publish routine. Decision:
stay on AOSP for a hobby project; switch to LineageOS if MatonOS ever becomes
commercial (monthly security updates would then be expected).

## Security updates

- AOSP: monthly Android Security Bulletin (patch levels YYYY-MM-01 / -05);
  merge the AOSP security tags monthly/quarterly. Mainline modules and
  WebView don't get Play updates here: they update only with our OS.
- Kernel: follow mainline stable (7.2.y) closely; move to the 2026 LTS.
- Mesa/minigbm/drm_hwcomposer: bump with OS releases.
- Apps (Fennec, Fossify, …): APK updates move to the
  [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md) APK Source.
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
- v1 builds are not secure builds (userdebug, historically permissive SELinux, AOSP test
  keys, no AVB). Secure releases (≈v3): `user` builds, enforcing SELinux,
  own release keys, AVB, signed shim/Secure Boot.
- **Versioning: Ubuntu-style `YY.MM.P`** (e.g. 26.12, point release
  26.12.1; internally 26.12.0). v1–v4 remain development milestones;
  releases are named by date. Set in the rename pass: `ro.vendor.matonos.version`,
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

- **Mesa driver set = host distro parity (user, 2026-09-26)**: every x86 PC
  driver the host's Mesa ships, minus non-PC ones (d3d12/WSL, asahi,
  gfxstream) and i915 (Gen3 GMA: GLES 2 only, too little for Android's UI;
  needs libpciaccess; llvmpipe covers it). GL: iris crocus radeonsi r600 r300
  nouveau virgl svga zink llvmpipe softpipe. Vulkan: anv hasvk radv NVK venus
  lavapipe.
  Intel anv/hasvk/none picked per PCI ID from a table generated out of Mesa's
  pci_ids (vendor/etc/intel_vulkan_pci_ids.txt); NVIDIA -> NVK. nouveau GSP
  firmware already staged by build-kernel.sh.

Priorities from the test devices:
- **v1 must-have**: crocus + hasvk, Intel generation check in
  pc-gpu-detect.sh (implemented 2026-09-26, untested on hardware); automatic display density from EDID (physical size) in
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
- **Preinstalled apps (historical v1.1 plan; store superseded)**: the earlier
  plan listed F-Droid + Privileged Extension; see the
  [MatonOS Software Centre](docs/SOFTWARE-CENTRE.md) design for the target
  app catalogue and updater. Remaining listed apps are Fennec F-Droid
  (browser), Fossify Gallery/Calendar/Contacts/Clock/Notes/
  Calculator/Music Player (replacing the AOSP ones), Open Camera.
  Not preinstalled: Fossify File Manager, QuickSearchBox, Dialer, Messaging,
  HeliBoard, Taskbar. Keyboard stays LatinIME. The earlier F-Droid
  "Install unknown apps" setup grant is historical; installer privileges and
  user confirmation follow the Software Centre design.
- **No-GPU fallback = vgem virtual GPU (decided 2026-09-27, user)**: Android always sees a GPU. Without a supported GPU, early boot loads the upstream `vgem` module (render node, shmem dma-bufs); Mesa renders with llvmpipe/lavapipe; minigbm treats vgem as a software GPU; drm_hwcomposer imports the buffers into the real KMS driver (simpledrm on real no-GPU PCs) or copies into dumb buffers where import isn't possible (bochs-drm). vkms not used (keeps real modes/EDID/hotplug). Replaces the gralloc "no render node" crash (SF "output buffer not gpu writeable").
- **rn-common comes back out of Settings (user, 2026-09-28)**: with a
  second Expo app coming (the Flatpak store / stub generator), the shared
  pieces move back to a shared library `rn-apps/rn-common` (Expo local-module
  package): the bridge client, SystemIcon/Symbol, Hardware API, strings/i18n
  helpers and shared Expo UI components that follow our React rules (no
  mount redirects, no complex JSX conditionals, no .map in JSX). Expo UI
  only — no react-native-paper. Settings and the store app both depend on
  it; supersedes "absorbed into Settings" below.
- **v2 starts with MatonOS Settings (decided 2026-09-27, user)**: an Expo app (SDK 57, CNG, Hermes) built ONLY with Expo UI Jetpack Compose (`@expo/ui/jetpack-compose`) — no react-native-paper at all (rn-common's paper parts become an opt-in subpath). It replaces the Java MatonOSSettings stub (same package org.matonos.settings and key). The installer is part of Settings (an "Install MatonOS" section, only on the live image), absorbing rn-apps/installer and the install/ service work; the separate installer app is dropped. Entry points: no app-drawer icon — a tile on Android Settings' homepage (IA_SETTINGS injection); on the live image only, an "Install MatonOS" drawer icon (disabled activity-alias enabled at boot when ro.boot.matonos.live=1). `rn-apps/rn-common` is absorbed into Settings as its local Expo module (bridge client, SystemIcon/Symbol, Hardware API; paper parts removed) — Settings is its only consumer; the parked Recents still references the old rn-common and won't build until revived.
- **Launcher = stock Launcher3 in desktop mode (decided 2026-09-27, user)**: the custom Shell/Shelf/Recents reimplemented what Android already has. Home, taskbar and recents are AOSP's Launcher3 (Launcher3QuickStep), the default display is forced desktop-first via WM Shell's `persist.wm.debug.force_desktop_first_on_default_display_for_testing` (debug property; if a release drops it, the classic touch-first shell is an acceptable fallback). Shell and shelf are retired and removed (2026-09-30); Recents, the bridge nav-host, and the Shelf/rn-common APIs (notifications, hardware, home/foreground, SystemIcon) are PARKED in the repo (commit 183aae4), not in the image. Superseded entries below (nav-bar provider, launcher split) are kept for reference.
- **Navigation bar = replaceable provider hosted by the system bridge (decided 2026-09-27, user)**: SYSTEM_ALERT_WINDOW overlays are force-hidden on screens with HIDE_NON_SYSTEM_OVERLAY_WINDOWS (Settings, permission dialogs, installer); SYSTEM_APPLICATION_OVERLAY is signature|recents|role|installer, so privapp XML can't grant it. Instead the platform-signed bridge owns a real system overlay window (bottom bar, reserves its insets) and embeds the provider app's UI via SurfaceControlViewHost (public API; UI + input stay in the provider's process). Provider = user's choice, default org.matonos.shelf; anyone can write their own bottom nav. Dangerous capability: default deny, explicit user selection, provider cert pinned, revocable, audit-logged; only ONE provider at a time; bridge falls back to hiding the bar (never a blank system window) if the provider dies.
- **Launcher split = three separately updateable Expo SDK 57 apps (decided 2026-09-26, user)**: MatonOS Shell owns fullscreen Home and Drawer; MatonOS Shelf owns the persistent overlay UI/window and Java fallback; MatonOS Recents owns the recent-task screen and task anchor. All three use `rn-apps/rn-common` and pinned Expo 57 / React Native 0.86.3 / React 19.2.3 / Hermes. Metro ports are 8081, 8082 and 8083, and each app has its own persistent signing key and Hermes runtime. Generated Android projects use CNG and each app's pinned Gradle wrapper.
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
  **Requirement: the Software Centre must be able to update every eligible
  preinstalled app.** So each APK comes from the exact repository and signer
  that the APK Source will trust (currently f-droid.org
  main repo; its signer, whether F-Droid's or the developer's for
  reproducible builds), is never re-signed, is never platform-signed or
  privileged, and stays a normal updatable app. Verify that each eligible
  package is discoverable and update-compatible through the Software Centre.
- Browser: preinstall **Fennec F-Droid** (Firefox rebuilt from Mozilla
  source, no Firefox trademarks/telemetry; x86_64 builds; updates through the
  Software Centre's APK Source) as a presigned prebuilt APK in `product` (not
  privileged, so normal PackageInstaller updates replace it). Official Firefox only if Mozilla's trademark/
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

- **systemd-appd compatibility (user, 2026-10-03).** systemd-appd (draft,
  systemd PR #43885, v263; Varlink `io.systemd.AppInstance`) identifies apps
  by the cgroup xattr `user.app_id` + SO_PEERPIDFD. r24: linuxd sets
  `user.app_id` on each app's cgroup. Later, when PipeWire or portals need
  appd: a per-session Varlink emulation in the compositor host (identity =
  stub/Flatpak id, permissions = the stub's Android grants). Research:
  out/pc-logs/agents/opencode-appd-result.md.

- **Software Center honours `required-flatpak` (user, 2026-10-03).** Flathub
  apps declare a minimum Flatpak version in their metadata
  (`[Application] required-flatpak=…`). The system Flatpak version (already
  reported up for the broker's minimum-version check) is passed to our
  Flathub store client through the System Bridge, so it can mark or hide apps
  that need a newer Flatpak than the device has, instead of failing at
  install time.

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
- **Out-of-tree kernel drivers (decided 2026-09-27, user): v4, via the
  add-on slot below.** Until then MatonOS ships upstream in-tree drivers only
  (Wi-Fi coverage on 7.2 is broad: rtw88/rtw89/rtl8xxxu, iwlwifi, mt76,
  ath9k/10k/11k, brcmfmac/b43). Users supply modules we can't ship (e.g.
  Broadcom `wl`) for their exact kernel release; signed with our key, stored
  where updates don't touch them, rebuilt per kernel release (DKMS-like).
  The Wi-Fi proxy targets nl80211/cfg80211 drivers; in v4 it also gains a
  wireless-extensions (wext) backend so legacy wext-only add-on drivers work
  behind the same stable wlan0 (user, 2026-09-27).
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

### Flatpak APEX updates (2026-10-04)
`com.matonos.flatpak` updates out-of-cycle (CVE fixes, newer Flatpak/bwrap
pair) via staged install (adb install for dev; the
[Software Centre APEX Source](docs/SOFTWARE-CENTRE.md) uses System Bridge and
PackageInstaller staged sessions). Same APEX key + higher version,
activates on reboot, apexd rolls back on failed boot. Live images (RAM /data)
cannot keep staged updates: they get new images. Release key: generate once,
keep offline (dev key in repo until then).

### Flatpak consolidation: static APEX, per-app installs, in-app D-Bus (2026-10-05)
- **Bionic stack deleted.** `retired/flatpak-bionic/` is gone and nothing
  references it. The only Flatpak is the static musl `com.matonos.flatpak`
  APEX: one multicall ELF for flatpak/ostree/bwrap, applets dispatch on
  `argv[0]` (Soong drops prebuilt-binary symlinks). linuxd calls the
  multicall directly for OSTree and the APEX wrapper for Flatpak.
- **Per-app installs, reuse stock Flatpak.** The preinstalled runtime app
  `org.matonos.linuxruntimes` owns the shared `--system` installation; each
  stub owns a `--user` installation at `/data/matonos/linux/apps/<uid>`.
  There is no global `/data/matonos/linux/flatpak*` installation. linuxd
  resolves an app's roots (`--user info` probe) and runs `uninstall`,
  `metadata`, `desktop_entry`, `icon` and `list_installed` through stock
  Flatpak against those roots. The bridge supplies the runtime app's UID for
  `add_flathub`/`list_remotes` so those target the same per-user install.
- **Launch is stub-driven.** The generic `run` command and caller-supplied run
  arguments were removed; the signed stub's ref + pinned commits drive
  `flatpak run`.
- **Appstream is stripped** from the Flatpak build; icons come only from the
  app's exported `export/share/icons/hicolor/...` tree.
- **Session bus/portals move into the app via dbus-java** (dbus-java-core
  5.2.2 + an Android `LocalSocket` transport, merged into the compositor
  host). `linux/dbus-broker` is transitional until that path is validated on
  device, at which point the native broker and its policy can be retired.
  There is no `xdg-dbus-proxy` in this setup.
- **Provenance.** The static helper sources were synced to MatonOS_apexs
  `e7fe3de`; the next full `build-static.sh` refresh updates the device
  `linux/flatpak/prebuilt/static/SOURCE` commit pin.
- **Install is two-phase because the app UID is late-bound (2026-10-05).**
  The stub APK is what gets the Android UID, so a first install has none:
  Flatpak installs into a temp directory under the software store's UID, the
  bridge generates and installs the stub from that deployment, PackageManager
  assigns the stub UID, and linuxd re-homes the install to
  `/data/matonos/linux/apps/<stub_uid>`. The shared runtime `--system`
  installation is owned by the preinstalled Runtimes app and is known up
  front. This supersedes any assumption that install receives the app UID.

