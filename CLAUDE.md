# MatonOS device tree — notes for Claude

Read `NOTES.md` first: roadmap, decisions (with reasons) and open issues.

## Principle: work around Android's quirks, don't patch into Android

When Android misbehaves on PCs, prefer solutions that sit *around* Android
over changes *inside* it. They survive AOSP upgrades (twice a year) far
better than hacks threaded through framework code. In order of preference:

1. Configuration: overlays (`overlays/`), build properties, SystemConfig
   XML, init rc, settings defaults.
2. Our own components outside Android's code: vendor daemons/HALs (e.g.
   `power/matonos-sleepd.c`, which owns sleep so Android's broken sleep path
   is never used), system_ext helpers (`setup/`), and later our own apps and
   services.
3. Only when there's no other way: a small, self-contained commit in our
   fork of that project (see "forks, not patch series" below; one concern per commit, gated by a `ro.matonos.*`
   property where it changes behaviour, with a comment saying why). Expect
   to re-check every such commit on each AOSP release.

Before patching Android, ask whether we can switch the behaviour off and
provide it ourselves instead.

## Rule: forks, not patch series (user, 2026-09-29)

Changes to other people's code go in real repository forks in the MatonOS-dev
GitHub org (branch `matonos/v1.2`, listed in FORKS.md and pulled in by
`manifest/maton.xml`), not in patch series inside this tree. Existing patch
series (`patches/`, `linux/third_party/*/patches`) are transitional: move them
to forks when touched. Don't start new patch series.
Our own code (daemons, HALs, sepolicy, the bridge, installer, rn-apps,
compositor, D-Bus broker, tools, docs) stays in this one MatonOS repository;
forks are only for other people's code.

## Long-term goal: no AOSP modifications except the system bridge

**Rule (user, 2026-09-24): if something can't be done without patching AOSP,
give that feature up for the time being** — no fallback patch. **The one
exception (user, 2026-09-25; microG moved to v3 on 2026-09-26):** microG signature spoofing, implemented exactly
like LineageOS (patch 0002: only microG-signed gms/vending may present Google's
signature; no bridge involvement, user 2026-09-28); patch budget = 1. Record it as
a known gap in NOTES.md ("Dropped for zero patches") with what it would need. **Also allowed (user,
2026-09-26): small, upstreamable fixes for obvious AOSP bugs** — currently
`patches/frameworks/base/0001` (unified battery icon hidden when no battery
is detected; the Compose icon ignores the icon hide list).

Target: stock AOSP framework and mainline modules, unpatched; MatonOS lives
in configuration, our own components (daemons, HALs, apps) and the one
system bridge app. Every new patch must justify why config, a daemon, a
spoofed/virtual device or the bridge can't do it. Retire existing patches as
replacements land:
- audio HAL patches (`hardware/interfaces`) → stock AOSP AIDL HAL plus ODM
  ALSA endpoint glue;
- Launcher3 + SystemUI classic-navbar patches → our own launcher;
- inputflinger absolute-mouse / PS/2-wake (`frameworks/native`) → an input
  daemon re-emitting odd devices via uinput as devices Android handles;
- Wi-Fi stubs (`packages/modules/Wifi`, `frameworks/base` 0003) → **spoofed
  Wi-Fi** (user preference): with no real adapter, matonos-wifid creates a
  `mac80211_hwsim` radio (no networks) so Android's own Wi-Fi stack and UI
  always run; `virt_wifi` (Ethernet shown as Wi-Fi) only in QEMU tests;
- Bluetooth stub + SystemConfig gate → **AOSP's stock Bluetooth HAL**
  (user, 2026-09-30): `android.hardware.bluetooth-service.default` binds a
  real controller over the kernel HCI user channel (MGMT discovery) and
  reports unavailable with no controller, so Bluetooth is cleanly off — the
  `matonos-btd` vhci emulator spoof is retired (it crashed the stack and hung
  boot on radio-less PCs);
- drm_hwcomposer / minigbm → our own forks via the local manifest (vendor
  components), fixes upstreamed;
- WM Shell desktop behaviours (keyboard+mouse → desktop, maximise =
  fullscreen) → virtual-touchpad spoof / launcher action via the bridge; if
  that fails, drop the behaviour for now and propose it upstream.

Kernel rule (user, 2026-09-24): prefer building on the kernel's **stable
userspace ABI** (syscalls, netlink/nl80211, uinput, hwsim's netlink medium,
sysfs) over our own kernel modules: inside the kernel there is no stable
API or ABI, so
out-of-tree drivers break on kernel bumps. A custom module only when
userspace can't do it; then tiny, built with our kernel, LTS, upstreamable.

**Target Wi-Fi architecture (user idea, after v1.2):** Android only ever
sees one virtual `wlan0`; our code does the real radio work. `wlan0` is a
stock `mac80211_hwsim` radio whose "air" is our daemon (hwsim netlink
medium, as wmediumd): it injects beacons of the networks the real
adapter(s) see, relays frames to/from the chosen real adapter (built-in or
dongle, hotplug invisible to Android), and our own supplicant HAL
(`ISupplicant` AIDL) receives SSID/password from Android and reports
connection state. Android keeps the IP stack and UI. Fallback only if frame
relay proves unworkable: a tiny userspace-backed cfg80211 proxy driver.
**Keep it thin (user):** the goal is a stable link interface for Android;
our code is a glorified proxy that forwards whatever it can unchanged
(scan results, auth/EAPOL frames so Android's own security runs end to end,
data, link metrics) in 99% of cases. Own behaviour only for the 1%: no
Wi-Fi hardware (spoofed empty radio) and a dongle unplugged/replaced
(wlan0 stays; Android sees a normal disconnect/"no networks", then the new
adapter's networks). No feature re-implementation beyond that.
Bluetooth equivalent: our HAL is the virtual interface, proxying to the
chosen real controller or the spoofed Rootcanal one.

## Principle: MatonOS owns the hardware glue (decided 2026-09-24)

**Preference order (user, 2026-09-25): first prize = a usermode driver
(our daemon in ODM) behind a stable device that AOSP's STOCK HAL / stack
already uses (Wi-Fi hwsim radio, Bluetooth virtual controller, ALSA audio
selection, input uinput devices); second prize = a custom HAL of ours.
Choose a custom HAL only when it brings a massive benefit, mostly
performance (e.g. latency, zero-copy camera/video paths).**

**Headline rule (user): per device class, Android sees one stable virtual
interface; our thin proxy behind it forwards whatever it can and alone
handles the mess** — hotplug, several devices, no device, replacing one.
- Wi-Fi: one virtual `wlan0` (see target Wi-Fi architecture).
- Audio (user decision 2026-09-25 ~21:00): use BayLibre's generic AIDL audio
  HAL, pinned at `a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5` in
  `hardware/baylibre/audio`. A timeout-limited asynchronous ODM selector picks
  the first non-Loopback/non-Dummy ALSA card with a playback PCM and sets
  `persist.vendor.audio.primary.card` / `.device`. Schema-7.0 ODM/vendor
  policy includes `default`, `r_submix`, `usb`, and `stub`; no Bluetooth/LE.
  No generated-policy detector, bind mount, or snd-aloop loader is shipped.
  Upstream's actual APEX name is `com.android.hardware.audio.generic` (its
  README says `...baylibre`); use the module name in its Android.bp until a
  fork can reconcile this. Upstream VINTF says core v3; this checkout has
  frozen core v4 and no v5, so the ODM override uses v4 and the configured
  instance set. **HARD RULE: audio never blocks boot.** If selection fails or
  no card exists, init sets the primary HAL's paced output/simulated-input
  properties; no selector result is a HAL startup prerequisite. Verify with
  HDA, without audio hardware, and with the selector failing.
- Camera: stable camera IDs (e.g. front/back) via our own thin camera HAL
  (userspace ICameraProvider, not v4l2loopback: out-of-tree kernel code)
  forwarding frames from whichever real webcam is chosen; plug/unplug
  swaps the source behind the same ID.
- Bluetooth: our HAL is the stable controller; behind it the chosen real
  controller or the spoofed one.
- Input: one stable virtual keyboard + one stable virtual pointer via our
  userspace input daemon (uinput): real keyboards/mice/touchpads/VM tablets
  merged and translated behind them (PC keys, Fn combos, quirk tables of our
  own), hotplug invisible to Android.

Android assumes a phone: one known audio chip, one GPU, one camera, fixed
sensors, nothing hotplugged. PCs have several of each, or none, and they
change. So **our own components discover, choose and manage hardware**, and
present Android with a simple, stable device through its normal HALs/APIs:

- A small daemon/script per area, in its own SELinux domain: enumerate
  devices, pick the one(s) Android gets (built-in before external at boot,
  keep the active one while present, persisted user choice wins, switch on
  hotplug), power/rfkill/firmware, then expose them the way Android expects
  (e.g. `wlan0`, one HCI device, one configured ALSA audio endpoint).
- Publish state as `vendor.maton.<area>.*` properties readable by the
  platform; control over a small binder/socket API guarded by a
  signature|privileged permission, for the v2 settings apps.
- Keep Android's own stacks and APIs (apps must keep working); replace only
  the "which device, and is it there" part.
- Existing/planned instances: `pc-gpu-detect.sh` (GPU → Vulkan/HWUI),
  matonos-sleepd (sleep), maton-audio-select (audio), matonos-wifid (thin
  channel server only; no spoofing);
  candidates below.
- Two kinds of hardware:
  - **Dynamic** (daemon decides on the fly what Android sees; changes reach
    Android as ordinary hotplugs): displays (forced connector status + EDID
    override), cameras (which webcams Android gets: filter IR/metadata
    nodes, pick front/back, USB hotplug), audio (stock HAL policy), Wi-Fi,
    Bluetooth.
  - **Static, known at boot** (detected once in early boot, then fixed
    config/properties for the HALs; no runtime switching): sensors
    (accelerometer/gyro/ALS on convertibles), battery, lid, and similar
    platform features (e.g. `pc-gpu-detect.sh`).

## Rule: our own apps and daemons are built outside Soong (decided 2026-09-24)

**Relaxed (user, 2026-09-28):** with zram (32 GiB) Soong analysis is no
longer the blocker it was, so Soong modules are allowed where they bring a
real benefit — e.g. native code that should link Android's platform
libraries (BoringSSL, curl, libxml2…) like the Flatpak stack. Still prefer
outside-Soong builds for code we iterate on constantly (our Gradle/Expo apps,
fast-changing daemons), and batch Android.bp changes into as few analyses as
possible. The original reasoning below still describes the trade-off.

Soong re-analyses (15–35 min, ~40 GB with swap) whenever the build graph
changes. So our **custom apps are never Soong modules**: they are Gradle
projects (`tools/build-apps.sh`) whose signed APKs the image imports with a
fixed `android_app_import` (privileged via the privapp allowlist, own keys,
not the platform key). Our **native daemons** likewise build with the NDK
(`tools/build-native.sh`, like Mesa/PipeWire) into `prebuilt/`, imported by
fixed `cc_prebuilt_binary` modules. Iterating on them then never triggers
analysis. Only what must be in AOSP's build stays in Soong: SELinux policy,
init rc, overlays, AOSP patches, HALs built from AOSP sources.

Exception (user, 2026-09-24): **one small system bridge app in Soong**
(`org.matonos.systembridge`, platform-signed, rarely changing, fixed file
set) is the only place that uses hidden/private framework APIs. It exposes
them over a stable AIDL interface to trusted callers only: permission
`org.matonos.permission.SYSTEM_BRIDGE` (signature|privileged) plus an
allowlist of our apps' signing certificates. Our Gradle apps use public
APIs + this interface, never hidden APIs directly.
**The bridge is the most security-critical piece of MatonOS** (a scoped,
modern "superuser" app: it grants named capabilities, never general
privilege). Rules: stay a thin gatekeeper (check + forward; daemons do the
work); default-deny per capability (own target name, allowlist entry and,
where relevant, user consent — nothing via general "trust") — **except
built-in MatonOS apps (user, 2026-09-27): apps shipped in the image and
signed with our pinned per-app keys get their capabilities allowed by
default; explicit user consent is only for third-party apps**; one audit log
of grants/denials/privileged calls, visible in settings; unit/CTS-style
tests for the authorisation logic; dangerous capabilities (signature
spoofing policy, installer, native-bridge/add-on installs, dev trust) in
separate modules with their own checks.
The bridge also carries a **generic channel to our daemons** (user idea):
`call(target, command, args) → result` and `subscribe(target, topic,
listener)`, forwarded as small JSON messages over a stable, VINTF-declared
AIDL interface `vendor.matonos.channel.IChannel/<target>` served by each
daemon (user decision 2026-09-25: Treble forbids system→vendor sockets;
formerly Unix sockets `/dev/socket/matonos/<target>`) (shared NDK helper library for
daemons). The bridge checks the caller (permission + cert allowlist +
per-target allowlist); SELinux lets only the bridge's domain connect to
daemon sockets; daemons validate strictly. New daemon commands/events then
need no bridge or AIDL change; typed per-area wrappers live in the Gradle
client library.

## Hard rule: Android never handles sleep (user, 2026-10-04)

Android's own suspend path stays disabled (config_useAutoSuspend=false,
screen never times out): when Android tries to sleep a PC it crashes.
matonos-sleepd alone decides when to suspend (idle timeout
persist.vendor.maton.sleep_idle_s, lid, power key) and writes the kernel
state itself. Never re-enable Android-initiated suspend to fix a sleep
problem; fix sleepd. Test VMs: set persist.vendor.maton.sleep_idle_s=0 after
boot (a suspended QEMU guest never wakes). Future: sleepd keeps adb alive
(e.g. no idle suspend while an adb session is connected on debug builds).

## Rule: newest libraries as the base (user, 2026-10-04)

When possible, build on the newest stable release of every third-party
library, toolkit and SDK we use (Flatpak, bubblewrap, xdg-dbus-proxy, GLib,
wlroots, Xwayland, Mesa, PipeWire, dbus-java, Gradle/AGP/NDK, npm/Expo
packages, API levels…). MatonOS releases every 6 months and each release has
to survive until the next one, so at every release cut we move to the newest
versions rather than keeping old pins. Pin by version + SHA-256/commit, and
keep our patches small enough to rebase each time (forks, not patch series).
Prefer the newest stable over prereleases; note any deliberate exception
(and why) next to its pin.

## Rule: Flatpak apps never get binder (user, 2026-10-04)

Linux/Flatpak app sandboxes must have no access to Android binder. The
mechanism is SELinux: executing bwrap transitions out of linuxd's domain
into the app sandbox domain, so bwrap and everything below it (the app,
its children, nested sandboxes) have no binder_use/binder_call/
service_manager rights, guarded by a neverallow. Binder device nodes that
are visible in the sandbox (e.g. via `devices=all`) are therefore unusable.
Everything Android-side goes through our sockets (Wayland/X11/D-Bus/portals)
to the compositor host and the System Bridge.
Bubblewrap is itself confined (user, 2026-10-04): two domains, each with
exactly the permissions it needs — exec of bwrap transitions linuxd into a
narrow bwrap setup domain (namespaces, mounts/pivot_root inside them,
uid/gid maps, reading the Flatpak deployment, binding the granted sockets
and devices, only the capabilities bwrap really uses); bwrap's exec of the
app payload (Flatpak deployment files with their own exec type) transitions
again into the app domain, which keeps none of the setup rights. Seccomp and
no_new_privs from Flatpak apply on top.
Inside the sandbox an app sees an ordinary user without admin rights
(user, 2026-10-04): /etc/passwd and /etc/group (generated by Flatpak) show
one normal unprivileged user (UID >= 1000, a real name/home — never root,
system or the raw Android UID) with no wheel/sudo/admin/adm/lpadmin
membership; no sudo/pkexec/su path; polkit is absent on the bus;
no_new_privs makes setuid useless. Granted extras (e.g. controllers) appear
as plain groups, never admin ones.
Principle (user, 2026-10-04): from the app's side, launching is a single
lever that says "start". The stub chooses nothing (no arguments, paths,
mount options, devices, environment or flags); the privileged side derives
everything from the verified stub identity (UID + per-device signature),
its installed manifest and its granted Android permissions.

Principle (user, 2026-10-04): containment beats compatibility. Default to
the tightest permissions; when an app breaks on a missing permission, fix
that case deliberately later (generically, never per app) rather than
loosening the sandbox up front.

## Rule: our native code is plain C (user, 2026-09-28)

New native code we write from scratch (daemons, helpers, services) is plain
C, like `native/matonos-sleepd/matonos-sleepd.c` — the user reads C far more
easily than C++. Use C++ only where a library forces it (e.g. AIDL NDK
backends, liblp), and then keep the C++ to a thin shim around C code.
Android-side app/system logic that isn't native goes in Java (not Kotlin).
Existing C++ is not rewritten just for this; trim or convert when touched.

## Rule: our drivers live in the ODM partition, invisible to Soong (user, 2026-09-25)

(Vehicle: Android's ODM partition — stock Android reads /odm/etc/init,
/odm/etc/vintf/manifest, /odm/etc/permissions, /odm/build.prop,
/odm/overlay and /odm/lib64 at runtime, so new drivers, HAL types,
features, properties and overlays need no Soong change. Built by
tools/build-bundle.sh as odm.img. Details below say `matonos` partition;
read "the ODM bundle".)
Properties: vendor-owned ones go in `/odm/etc/build.prop`; runtime-writable
system ones are set at runtime (daemons / system bridge); boot knobs as
`androidboot.matonos.*` kernel-cmdline values (→ `ro.boot.matonos.*`, in our
loader entries). Only system-owned read-only props (init refuses them from
vendor/odm) stay in product .mk — design new tunables to avoid those.

All our userland drivers/daemons/HAL binaries (+ their rc files, configs,
libs like libmatonos-ipc) ship in our own read-only erofs partition
`matonos`, built by our own script (`tools/build-bundle.sh`) from the
NDK outputs and packed into `super` by make-live.sh (later Install me /
Updater). One-time AOSP-side hooks only: fstab mount, ONE fixed init rc that
imports the bundle's `etc/init/` directory, ONE file_contexts pattern for the
bundle (→ matonos_driver labels), fixed VINTF entries per HAL type. Adding
or changing a driver = rebuild the bundle + repack the image: no Soong, no
`m`. Privileged apps can't live there (Android only scans system,
system_ext, product) — they stay fixed prebuilt imports.

## Rule: one SELinux domain for all MatonOS userland drivers (user, 2026-09-25)

Adding policy FILES changes Soong's graph (full re-analysis); editing
existing ones doesn't. So all our daemons/HALs (wifid, btd, inputd, sleepd,
PipeWire proxy, our HALs, future display/camera daemons) run in ONE domain,
`matonos_driver`, with a FIXED policy file set in `sepolicy/matonos/`
(`matonos_driver.te`, `matonos_bridge.te`, `file_contexts`, `property.te`,
`property_contexts`, `service_contexts` — the last added once for the stable
AIDL channel). `tools/preflight.sh` enforces the set. Separately, `sepolicy/vendor/` (labels for AOSP components
we configure: graphics HALs, block devices, Mesa/gralloc libs, pc-gpu-detect,
pc-wakeup) must stay in BOARD_VENDOR_SEPOLICY_DIRS.
Labels by pattern: `/vendor/bin/(hw/)?(matonos|maton)-.*` → the driver exec
type; `/dev/socket/matonos/.*` → one socket type only the bridge may connect
to; properties under `vendor.maton.` → one platform-readable type. New
daemons = edits to these files, never new files. Trade-off accepted by the
user: less isolation between our own daemons; the MatonOS↔Android boundary
stays. Exception: high-risk services get their own domain, still
defined inside the fixed files (a new type in matonos_driver.te is not a new
file): `matonos_installer` (v2 install service, raw block-device write, live
image only) — the shared driver domain never gets disk-wiping rights. The system bridge (platform app side) keeps its own small policy.

## Checklist: adding or changing anything init starts

Every new vendor service, HAL, or `exec`'d script needs **all** of these, or
init refuses to start it — even with SELinux permissive ("has incorrect label
or no domain transition"). This silently cost a boot cycle once (drm_hwcomposer
and the minigbm allocator never started, so SurfaceFlinger waited forever).

1. `sepolicy/vendor/file_contexts`: label the binary/script with an `*_exec`
   type. Prefer AOSP's existing HAL domains (`hal_<name>_default_exec`, see
   `system/sepolicy/vendor/`); otherwise add a domain `.te` file
   (`type x, domain; type x_exec, exec_type, vendor_file_type, file_type;
   init_daemon_domain(x)`).
   Platform-side helpers (system_ext, e.g. `setup/matonos-setup.sh`, which
   runs `/system/bin/cmd`; vendor code may not) go in
   `sepolicy/system_ext/private` as a `coredomain` with a `system_file_type`
   exec type.
2. Libraries loaded into app/system processes (gralloc mapper, Mesa, Vulkan
   HALs, …) → `same_process_hal_file`.
3. Run `tools/check-selinux-labels.sh` after building (also run by
   `tools/build.sh`); it must report no FAIL.
4. Before switching SELinux to enforcing: write the real allow rules for new
   domains (they're minimal while the device is permissive).

## Other traps already hit (see NOTES.md for detail)

- `prebuilt/` is shared: `build-kernel.sh` owns bzImage/modules/firmware,
  `build-mesa.sh` owns `prebuilt/mesa`. Never `rm -rf prebuilt/` wholesale.
- AOSP host tools (`out/host/linux-x86/bin`) must go *last* in PATH in our
  scripts; AOSP's `sgdisk` is a cut-down build.
- Don't edit a bash script while a background job is running it.
- `pgrep -f`/`pkill -f` patterns match the calling shell's own command line;
  use the `[x]yz` bracket trick or `pgrep -x`.
- A module in a directory with `soong_namespace {}` is **silently skipped** in
  `PRODUCT_PACKAGES` unless that directory is in `PRODUCT_SOONG_NAMESPACES`.
  After adding packages, check they landed in `out/target/product/*/vendor`.
- `lunch` puts `out/host/linux-x86/bin` first in PATH; run envsetup/lunch
  under bash (zsh misparses the target), not in the caller's zsh.
- `overrides` (e.g. our apps replacing AOSP ones) only drops modules from the
  install list; their old files stay in `out/target/product/*/` staging and
  still land in the images. Run `m installclean` after adding overrides.
- Preinstalled APKs (`apps/`) must stay byte-for-byte (F-Droid updates):
  Soong's presigned-APK check wants `skip_preprocessed_apk_checks` exactly on
  APKs that fail it (compressed JNI; priv-apps also compressed dex).
  `tools/fetch-apps.sh` verifies this before a build.
- Memory: run builds in a memory-limited scope so they spill to swap instead
  of starving the host: `tools/build.sh` does it itself; for ad-hoc builds use
  `systemd-run --user --scope -p MemoryHigh=16G -p MemorySwapMax=infinity bash -c '…'`.
- Build speed: export `SOONG_INCREMENTAL_ANALYSIS=true` for every `m`
  (`tools/build.sh` does), so an Android.bp change doesn't re-analyse the
  whole tree. For one component use `tools/build.sh -m SystemUI` (no
  images; same full-product graph as image builds). Don't use
  `SOONG_PARTIAL_ANALYSIS` routinely: each change of its module list, and
  each switch back to a full build, re-runs the whole ~18 GB analysis —
  with several agents alternating builds that meant a full analysis almost
  every time. Analysis itself can't be skipped after
  Android.bp/product-config changes; plain rebuilds already skip it.
- **Only the coordinating agent (Claude) starts builds** (user rule): build.sh
  refuses without `MATON_BUILD_COORDINATOR=1`. Codex sub-agents append a
  request to `out/pc-logs/agents/build-requests.txt` and read
  `out/pc-logs/agents/build-status.txt`. Build RAM cap: 20 GB (`MemoryHigh`,
  then swap; `MATON_BUILD_MEM_HIGH` overrides). Soong ANALYSIS and the agents
  never run at the same time (user rule; Kati/compile are fine): the
  coordinator builds via `out/pc-logs/agents/coord-build.sh`, which freezes
  (SIGSTOP) all `codex exec` process trees until analysis is over.
- **zram is a build requirement (user, 2026-09-25)**: build.sh refuses to run
  without an active zram swap >= 16 GiB at the highest swap priority (setup
  commands in its error message; this host: 32 GiB zstd, prio 200, ~6x
  compression, Soong analysis ~11 min). `MATON_SKIP_ZRAM_CHECK=1` bypasses.
- **Tree pruning is OFF (user, 2026-09-25)**: it caused repeated late build
  failures (hidden Soong plugins, team modules, Trusty dirgroups, and real
  product dependencies in platform_testing, hardware/google, cts/libs, Car,
  cuttlefish/apex, goldfish surfacing only at ninja time under
  ALLOW_MISSING_DEPENDENCIES). We accept slower analyses instead. The tool
  and list stay for reference; don't re-enable without checking every
  "missing dependencies" rule of non-test modules in out/soong/build.*.ninja.
- Build RAM/time: `tools/prune-tree.sh apply|revert|status` hides ~30% of
  the tree's Android.bp files (test suites, automotive, cuttlefish, other
  vendors; list in `tools/prune-tree.list`) from Soong via `.find-ignore`
  markers — nothing is deleted. While pruned, build.sh sets
  `ALLOW_MISSING_DEPENDENCIES=true`. Bare `m` in a pruned tree needs it too.
- ccache: build.sh wraps C/C++ compiles with ccache when it is installed;
  cache in `out/ccache` (40 GB). It must be under out/: the build sandbox
  makes everything else read-only ("ccache: error: Read-only file system").
  Toggling it recompiles all C/C++ once. `MATON_CCACHE=0` disables.
- `generic_system.mk` artifact-path check: nothing from the device tree may
  install into /system. `PRODUCT_PACKAGES += <lib>` installs the lib's
  /system variant too (libffi did), and a plain `android_app` lands in
  /system/app — both fail Soong analysis only at its very end (~40 min).
  Mark our modules `vendor: true` / `system_ext_specific`, and never add
  AOSP libraries to PRODUCT_PACKAGES to "make sure" a vendor copy exists.
- Our Gradle APKs are imported with `preprocessed: true` (byte-for-byte, own
  key), NOT `certificate: "PRESIGNED"` (Make convention; Soong then looks
  for build/make/target/product/security/PRESIGNED.x509.pem and fails at
  ninja time). Keep dex stored + zipaligned in build-apps.sh so no
  `skip_preprocessed_apk_checks` is needed.
- Soong incremental analysis can panic after heavy graph churn
  (`panic: runtime error: growslice: len out of range` in
  `writeIncrementalModules`, after the full ~40 min). Recovery: one build with
  `SOONG_INCREMENTAL_ANALYSIS=false` (build.sh honours it).
- Public sepolicy (`*_PUBLIC_SEPOLICY_DIRS`) may only DECLARE types/attributes:
  macros like `app_domain()` / `init_daemon_domain()` emit type transitions,
  which break `version_policy` ("typetransition unsupported statement in
  attributee policy") late in the compile. Put them, and `typeattribute X
  coredomain;`, in private policy.
- Never edit a script that is running (coord-build.sh included): bash reads
  it incrementally and executes garbage at the old offset. Copy + `mv`.
- Frozen stable AIDL (`aidl_api/<pkg>/<N>/`): editing a frozen version fails
  at compile time ("Modification detected of stable AIDL API file"). Only
  before it ever shipped: recompute `.hash` =
  `(cd aidl_api/<pkg>/<N> && find ./ -name "*.aidl" -print0 | LC_ALL=C sort -z
  | xargs -0 sha1sum && echo latest-version) | sha1sum`. After shipping: add
  version N+1 instead.
- VINTF manifest fragments (`etc/vintf/...xml`) must not be in
  PRODUCT_COPY_FILES (Kati error after the whole Soong analysis): add them to
  `DEVICE_MANIFEST_FILE +=` in the area's BoardConfig.mk (or a
  `vintf_fragments` property on a Soong module).
- Kernel options Android needs but distro configs leave off:
  `CPUSETS_V1`, non-empty `ANDROID_BINDER_DEVICES`, legacy iptables.
  Android's config fragment for 6.12 predates some of them.
