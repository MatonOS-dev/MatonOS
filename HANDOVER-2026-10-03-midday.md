# Handover — 2026-10-03 midday (GPU acceleration, r19–r21)

Supersedes the "NEXT" section of `HANDOVER-2026-10-03.md`. All work is
committed on `main` (device repo) and pushed only for minigbm. No release.

## READ FIRST — state at hand-off

- **r21 was building at hand-off** (`out/pc-logs/agents/build-repair-r21.sh`,
  log `out/pc-logs/repair-r21-build.log`, status in
  `out/pc-logs/agents/repair-image-status.txt`; image
  `~/matonos-images/matonos-live-repair-20261002-r21.img`, SHA in
  `out/pc-logs/repair-r21-sha256.txt`). User copies it to
  `~/matonos/user-vm.img` to boot. r21 is **untested**.
- r21 = r20 + X11 GPU acceleration + every compositor fix below. The user's
  VM still runs r20 with an installed compositor APK update (lost on reboot).
- Device repo HEAD at hand-off: `5760c8a` (plus this doc). minigbm fork
  `MatonOS-dev/android_external_minigbm` `matonos/v1.2` = `9e5ad57` (pushed).

## What r21 should show (test list for the user)

1. X11 apps (Brave, MC launcher, Steam) render with the GPU: logcat
   `MatonWLR` shows Xwayland starting with glamor; no
   `MatonXwaylandEGL` errors. If Xwayland exits, capture its stderr with the
   compositor-namespace wrapper trick (see "Debug recipes").
2. Firefox (Wayland, GPU since r20): menus/hover/History, scrolling.
3. shm apps and Xwayland apps keep updating (no freezes).
4. Several X apps at once (each session has its own Xwayland).
5. Steam: "kinda works"; `steam-devices` (udev rules for controllers) is not
   applicable on Android — controller access would be a separate roadmap
   item (input daemon + Flatpak device access). Ask the user what else fails.

## Architecture now (zero-copy GPU)

- **minigbm fork (`9e5ad57`)**: a cros_gralloc handle with `id == 0` is an
  external dma-buf: `retain()` keys it by the dma-buf's dev/inode, metadata is
  process-local defaults. Test: `linux/compositor/native/tests/ahb_import_test.c`
  (stock minigbm fails 3 checks, fork passes).
- **Compositor** (`linux/compositor/native/`):
  - `dmabuf_import.c`: client dma-buf → id-0 handle →
    `AHardwareBuffer_createFromHandle` (method CLONE = **3**, vndk header),
    cached per wlr_buffer (addon).
  - `maton_renderer.c`: wraps pixman; dma-buf textures are stubs that lock
    their source buffer (release waits for SurfaceFlinger).
  - `presenter.c`: each window's scene → one child ASurfaceControl per buffer
    (geometry, z, alpha, opacity). dma-bufs shown directly; shm copied into a
    4-buffer pool per layer (free == `n_locks == 0`!). Retries when busy.
  - `compositor_core.c`: linux-dmabuf v4 (+feedback, real-import check),
    monitor sized to the display from Java (window id 0 resize) and grown to
    cover windows, xwayland_shell_v1 global filter per session, X11
    override-redirect overlays, X windows mapped/unmapped via XTrack,
    X window activation for focus, value120 scrolling.
- **linuxd**: `--device=dri`, no `LIBGL_ALWAYS_SOFTWARE`; sepolicy lets
  `matonos_linuxd` open the render node.
- **Xwayland GPU (r21)**: `linux/compositor/xwayland-egl/` generates
  forwarding `libEGL.so`/`libGLESv2.so`/`libgbm.so` (x86_64 trampolines) that
  load Mesa via `android_load_sphal_library`; libEGL maps
  `EGL_PLATFORM_GBM_MESA` to `EGL_EXT_platform_device` by render node (Mesa's
  Android build has no GBM platform; user wants **one Mesa only**). Shipped in
  `/system_ext/lib64/xwayland`, on Xwayland's `LD_LIBRARY_PATH` only.
  libepoxy 1.5.10; Xwayland `-Dglamor=true -Ddri3=true`. Device probe
  (`xwayland-egl/probe.c`) passed on virgl (RX 6600 host).

## Open items

Codex review (`out/pc-logs/agents/review/codex-review.md`), not yet fixed:
- #2 shutdown/allocation-failure paths unlock buffers on foreign threads;
  #3 release-fence fallback treats unsignaled fences as done.
- #4 minigbm id-0 dedup keys on dma-buf only; different views of one
  allocation share metadata.
- #6 X windows: a `Window`+scene leaks per map/unmap cycle.
- #7 presenter ignores buffer transforms and fractional viewport crops.
Other:
- Faster iteration without full images: `adb remount` overlay and/or a
  `tools/quick-image.sh` that repacks only changed partitions (user asked;
  not started).
- Portals (Steam/Firefox D-Bus warnings), D-Bus machine-id warnings.
- X11 menus cannot extend past the window edge (X positions itself).

## Debug recipes (worked today)

- Compositor-only test: `tools/build-apps.sh` with
  `MATON_APPS_ONLY=matonos-wayland-host MATON_APPS_FORCE=1`, then
  `adb install -r prebuilt/apps-built/MatonWaylandHost.apk` (restarts the
  compositor, closes Linux windows; lost on reboot — /data is RAM).
- Replace a file only inside the compositor: `nsenter -t <pid> -m --
  /system/bin/sh -c 'mount --bind …'` (app namespaces are slaves; verify
  `grep … /proc/1/mountinfo` is 0). For private test namespaces use
  `unshare -m` + `mount -o rprivate none /` first (a leak emptied
  /system_ext once today).
- Xwayland stderr: bind a wrapper script over `/system_ext/bin/Xwayland`
  in the compositor namespace that execs the real binary (bound elsewhere)
  with `2>>log`. A failed X session's socket is gone until the compositor
  restarts.
- Per-app env: write `/data/matonos/linux/flatpak/overrides/<app-id>`
  (`[Environment]` …), e.g. `WAYLAND_DEBUG=1` → trace in
  `/data/matonos/linux/cache/launch-<app>.log`. Remove when done.
- Codex: `codex exec -C <repo> -s workspace-write --add-dir <out dir>`;
  do **not** `--add-dir external/minigbm` (its repo `.git` symlink breaks the
  sandbox) — pass patches as files.
