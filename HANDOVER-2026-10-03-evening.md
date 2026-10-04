# Handover — 2026-10-03 evening (r22 → r23, r24 plan)

Supersedes `HANDOVER-2026-10-03-midday.md` for state; keep it for the GPU
architecture and debug recipes. Device repo `main` is pushed to
`MatonOS-dev/MatonOS`; minigbm fork `matonos/v1.2` = `349fa60` (pushed).

## READ FIRST — state at hand-off

- **r23 was building at hand-off** (`out/pc-logs/agents/build-repair-r23.sh`,
  log `out/pc-logs/repair-r23-build.log`, status
  `out/pc-logs/agents/repair-image-status.txt`, image
  `~/matonos-images/matonos-live-repair-20261002-r23.img`, SHA in
  `out/pc-logs/repair-r23-sha256.txt`). It was at 95% of the partition
  build. If it failed, read the first `FAILED:` in the log. r23 is
  **untested**.
- **r22** (`a9c23429…`) was tested today: Chromium on the GPU under X11 worked
  once the fixes below were applied by hand.
- **Agents running at hand-off:** opencode dependency/patch audit →
  `out/pc-logs/agents/opencode-deps-result.md` (input for r24 item 12).
- The QEMU VM is closed. Boot r23 with
  `MATON_QEMU_WINDOWED=1 MATON_QEMU_SOUND=virtio tools/run-qemu-live.sh -i … -a 5555`
  (`-x "<args>"` for raw QEMU arguments; trailing args are ignored).

## What r23 contains (test list)

1. **Codecs:** `media.c2.hal.selection=aidl` (device.mk). Before, MediaCodecList
   had only `c2.android.inproc.aac.decoder`: MP3 silent, YouTube would not
   start, VLC silent. Test: Music Player plays `/product/media/matonos/test-audio/ovmuz.mp3`,
   YouTube plays. `dumpsys media.player` lists the c2.android.* decoders.
2. **X11 on the GPU:** Xwayland `-glamor es` (WLR_XWAYLAND_GLAMOR, wlroots
   patch) + GLX via glamor's EGL provider (`xwayland-egl-glx.patch`).
   Chromium `chrome://gpu`: hardware accelerated (seen on r22).
3. **Compositor isolation:** decoration mode only after initial commit
   (was a wlroots assert killing all Xwayland sessions); xwayland_shell
   globals hidden until owned (a second X app killed the first); busy shm
   pool retried on a 50 ms timer (one stalled window froze all others).
4. **One app can't blank Android:** gralloc4 XRGB8888 plane layout (minigbm)
   + runtime-verified dma-buf formats with per-window copy/hide fallback.
5. **D-Bus at the compositor layer:** the compositor host starts one C broker
   per session (socket `bus` beside `wayland-0`); the broker binary ships in
   **MatonWaylandHost.apk** (`lib/x86_64/libmatonos-dbus-broker.so`), so
   D-Bus changes update with `adb install -r`. flatpak-portal stays native
   under linuxd (supervisor, peer-credential/pidfd ownership check). Only
   compatibility check: **system Flatpak ≥ 1.14.10**. Unprovided services
   (systemd, logind, PackageKit) answer ServiceUnknown; existing but
   disallowed ones AccessDenied. Settings portal Read double-wraps + ReadOne.
   machine-id in every sandbox; Inhibit portal (wake lock + keep-screen-on).
6. **Controllers:** runtime permission `org.matonos.permission.GAME_CONTROLLERS`
   → gid 2900 `vendor_game_controllers` on /dev/hidraw*, /dev/uinput;
   requested by stubs whose manifest has `devices=all|input`; udev database
   at `/run/udev` in sandboxes; SELinux labels `sysfs_matonos_input` /
   `sysfs_matonos_discovery` (enforcing-ready, policy compile passed).
7. **Quick Settings:** brightness + media-volume sliders via SystemUI flags
   (scene_container, dual_shade, qs_tile_detailed_view,
   expanded_audio_detailed_view, desktop_sizing) in `aconfig_value_set-matonos`
   — the flags are compiled READ_ONLY, so runtime `device_config` cannot set
   them. Plus a Media volume TileService in MatonOS Settings.
8. Journal socket per session (Waylyrics), stub package names for ids with
   `-` (cpu-x), LibreOffice broker dialog gone.

Check first on boot: compositor/broker startup (`logcat | grep -iE "Maton|broker"`),
a Flatpak app opening, `chrome://gpu`, MP3/YouTube sound, QS sliders.

## Decisions made today (also in memory / NOTES.md)

- **Bridge architecture stays:** compositor host → System Bridge
  (SYSTEM_BRIDGE permission, `ownsStub(uid, ref)`) → linuxd. The host uses
  `Binder.getCallingUid()` (audited).
- **One D-Bus bus per app.** Cross-app features go through Android
  (MPRIS↔MediaSession, OpenURI→intents, tray→notification). The broker
  moves to Java gradually; new portals are Java in the compositor host.
- **Audio:** real PipeWire + pipewire-pulse in pass-through mode, one AAudio
  stream per app stream (clean-room node modelled on Termux's
  module-aaudio-sink behaviour), Android mixes. Revive `tools/build-pipewire.sh`.
- **Flatpak apps per-app hacks are banned** ("if app X, change behaviour").
- **No prior-art credits without a provenance audit.** Audits of
  wlroots-android-bridge, Xtr126/labwc(-android) and Termux found no copied
  code (`out/pc-logs/agents/opencode-provenance*-result.md`). The README
  credit line was removed; re-adding it is the user's call.
- Linux-app layer moves to its own repository long-term (NOTES roadmap).
- Digitalis (ARM64 native bridge) is next after Flatpak work: source
  integration, AOSP 16→17 port medium, maintenance medium-high.
- Podman: future addon (system service + socket for Podman Desktop).
- xwayland-egl trampolines are a per-arch macro (`forward.h`: x86_64,
  arm64, riscv64).

## r24 plan (memory: r24-plan.md)

1. Bus layer follow-ups; Java portals: manifest `[Session Bus Policy]`,
   MPRIS↔MediaSession, OpenURI, NetworkMonitor, Notification.
2. App owns its Linux processes: linuxd moves sandbox processes into the
   stub's cgroup (`uid_X/pid_Y`) and sets `user.app_id` (systemd-appd
   readiness); Linux exit finishes the stub; ✕ = graceful close, Recents
   swipe = kill.
3. Sleep = cached-app freezer + xdg_toplevel `suspended`.
4. Audio (Android audio verified on virtio-sound and HDA): PipeWire
   pass-through + AAudio node, started per session by the compositor host.
5. Dependency updates: bubblewrap 0.10.0 → latest (GHSA-pxhw-h44j-8pfx,
   CVE-2026-41163) and the whole Linux stack per the deps audit; check how
   our patches rebase.
6. Later: gamepads via Android InputManager → virtual evdev pad per sandbox;
   clipboard via ClipboardManager.

## Lessons from today

- `tools/build-apps.sh` does NOT rebuild compositor native libs: run
  `linux/compositor/build-compositor.sh` first (stale libs caused a false
  "regression").
- Never `pgrep -f`/`pkill -f` for agents or builds (it froze my own build
  shell once today); list with `ps -eo pid,stat,args`, signal literal PIDs.
- Background launches of codex/opencode: `setsid nohup … < /dev/null &`.
  opencode work runs inside `opencode serve` (no `opencode run` process).
- Board-config changes (config.fs, BoardConfig.mk) trigger near-full rebuilds.
- VM `/data` is RAM: big Flatpaks fill it; `adb install` then fails.

## Update after r23 boot (late 2026-10-03)

r23 (`de877625…`) booted. Sound works (MP3/codec fix confirmed). The new QS
shade opens by clicking the clock (notifications) / status icons (QS), not
by dragging — accepted for now. **Flatpak apps broke:** CompositorService
crashes with `UnsatisfiedLinkError: libmaton_compositor.so not found`,
because `useLegacyPackaging = true` (added to ship the broker as an
executable) compresses JNI libs, and PackageManager does not extract native
libs for preinstalled system apps. Workaround on a running VM:
`adb install -r prebuilt/apps-built/MatonWaylandHost.apk` (update in /data
gets extracted). Proper fix: codex job `codex/broker-exec`
(`out/pc-logs/agents/codex-broker-exec-result.md`), then an r23b build.
After the `adb install -r` workaround: Door Knocker opens (compositor-layer
D-Bus works on device). Firefox asked for GAME_CONTROLLERS on launch — expected
(Flathub Firefox declares devices=all); prompt + manifest gating confirmed,
real controller access untested (no controller passed through).
Chromium fails ("Failed to get portal proxy: Connection reset by peer"):
likely xdg-dbus-proxy (apps with [Session Bus Policy]) cannot reach the
bus address unix:path=/proc/<supervisor>/fd/198/bus inside its helper
sandbox. Codex job codex/bus-proxy (VM access) →
out/pc-logs/agents/codex-bus-proxy-result.md.

## 2026-10-04 morning progress (r24 work; no r23b)

Merged and pushed to main: Chromium bus fix (a67bbff, verified on headless
VM 5562), system-app JNI/broker extraction (25d16ad), review fixes H8/H9/M2
(8211a9f; seccomp runtime test pending r24 image), D-Bus Java step 1
(a8bc215: Settings/Inhibit/OpenURI in Java), H1 enforcing by default
(7c96027; build the r24 test image with MATON_SELINUX_PERMISSIVE=1 and run
sepolicy/r24-enforcing-test-plan.md separately), dependency updates
(e822e68: bubblewrap 0.13.0, Flatpak 1.18.4, xdg-dbus-proxy 0.1.9),
compositor minSdk/targetSdk 36 (793fbf4), CLAUDE.md newest-libs rule.
Running: codex/app-owns (H6 per-app UID/domain + process ownership + sleep).
Then: H5 (controllers via InputManager), r24 image, test on VM 5562.
Java port step 2 = dbus-java 6.x + own Android LocalSocket transport.

## 2026-10-04 10:20
- Codex hit its usage limit (until 13:10). Code-storage design (NOTES
  "Flatpak code storage and execution") now runs on opencode
  deepseek/deepseek-v4.1-flash in worktree app-owns
  (out/pc-logs/agents/opencode-app-store-result.md pending).
- H5 design done (out/pc-logs/agents/opencode-h5-result.md): linuxd-owned
  uinput pad per session, only its node bound into that app's sandbox,
  presented as an Xbox 360 controller; stub forwards InputManager events.
- Model trial (qwen/qwen3-coder-next vs openai/gpt-oss-120b) on two small
  r24 fixes in worktrees trial-*, reports out/pc-logs/agents/trial-*-result.md.
- Headless test VM 5562 restarted on fresh r23 (permissive dev mode).
