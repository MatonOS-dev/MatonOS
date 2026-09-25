# Input absolute-pointer mapping — 2026-09-25 update

## Changes

- `matonos-inputd` prefers Android's active logical display size, then the
  current DRM connector mode, then fb0's current virtual size. It polls once
  per second for mode/size changes, maps absolute coordinates to that size,
  tracks the last target, and emits target differences as REL events. It keeps
  density compensation and never uses the DRM advertised `modes` list.
- The system bridge sends `Display.getRealSize()` to inputd over the existing
  IChannel `set_display_size` command on startup and display changes, retrying
  if inputd is not registered yet. The bridge starts at boot and
  reapplies acceleration disabled plus pointer speed -7 (unit gain) for the
  current user. The authorized `input.set_absolute_pointer_mode` operation
  does the same. These are global mouse settings because stock InputReader
  has no per-device acceleration control.
- `run-qemu-live.sh` advertises `-r` through virtio-gpu EDID. Interactive GTK
  launches now start fullscreen before POST and use `zoom-to-fit`, so the
  initial host display is already sized before Android chooses its mode;
  later host resizes scale the image. The agent harness remains headless.

## Verification

- NDK `build-native.sh`: passed; log at `out/pc-logs/input/build-native.log`.
- `preflight.sh`: passed after removing the obsolete display-size property
  rules.
- `bash -n tools/run-qemu-live.sh`: passed.
- Local QEMU 10.2.1 device help confirms `virtio-vga-gl` accepts `edid`,
  `xres`, and `yres`. QEMU's GTK backend documents `zoom-to-fit`, which scales
  the guest display to the window rather than resizing the guest mode.
- Fresh QEMU boot with `-r 1920x1080` reports `sys.boot_completed=1`,
  `wm size: Physical size: 1920x1080`, and `dumpsys display` real size
  1920x1080. No `video=` kernel argument was used. Evidence: `out/pc-logs/input/display-mode-1920x1080-evidence.txt`.
- QEMU's installed GTK documentation supports `full-screen=on` and
  `zoom-to-fit=on`; `bash -n tools/run-qemu-live.sh` passes after enabling
  fullscreen for interactive launches. The boot-size measurement above used
  the headless agent renderer, so the GTK pre-boot sizing behavior still needs
  a windowed/user-session check.
- Real-PC mode selection checked in the local drm_hwcomposer source: it
  selects the first DRM mode flagged `DRM_MODE_TYPE_PREFERRED`, then
  `HwcDisplay::Init()` applies that config. This covers EDID preferred/native
  modes; physical-machine runtime verification remains pending.
- Fresh-image corner tracking test remains pending. Display resolution was
  validated separately on the 18:31 image; absolute-pointer translation still
  awaits a fresh image with the IChannel geometry update.

## Remaining

1. Coordinator builds the requested image, including the bridge IChannel
   update and input daemon.
2. Boot a fresh windowed VM with `-r 1920x1080` on a free agent port. Confirm
   `dumpsys display` reports 1920x1080, then test each window corner, repeated
   edge hits, and re-entry for drift. Never touch the user's VM on port 5555.
3. Save display/input dumps, bridge and daemon logs, and screencap evidence in
   `out/pc-logs/input/`.

No AOSP patches or kernel changes were made. The global acceleration setting
may make relative mice slower than the prior accelerated default; verify
physical relative mice after the corner test.

# Mouse wheel follow-up — 2026-09-25

## Change

The stable pointer advertises high-resolution vertical and horizontal wheel
axes when the kernel uinput interface supports them. This made the prior
pass-through incorrect for legacy PS/2 sources: stock InputReader prefers a
high-resolution axis when the virtual device advertises one and ignores its
legacy axis, so a forwarded `REL_WHEEL` alone produces no Android scroll.

inputd now detects each source's high-resolution capability. It forwards
source `REL_WHEEL_HI_RES` and `REL_HWHEEL_HI_RES` unchanged, converts legacy
`REL_WHEEL` / `REL_HWHEEL` detents to high-resolution values at 120 units per
detent, and filters paired legacy events when a high-resolution event arrives
in the same frame. If a source advertises high-resolution support but sends
only legacy values in that frame, inputd flushes the legacy fallback at
`SYN_REPORT`. On kernels where uinput cannot advertise a high-resolution axis,
sub-detent input is accumulated and emitted as whole legacy detents.

The daemon logs source wheel capabilities at discovery and logs the first
forwarded vertical/horizontal wheel event, including its source and units.
This gives evidence for raw input reaching inputd even though `EVIOCGRAB`
prevents a second reader such as `getevent` from consuming events from that
same physical node. `getevent -l` on the stable virtual pointer can show the
normalized output.

## Verification status

- NDK `build-native.sh`: passed; see `out/pc-logs/input/build-native.log`.
- `preflight.sh`: previously passed; rerun after removing the obsolete display
  property policy below.
- Fresh-image tests are pending the coordinator's queued image build. On that
  image, first stop inputd to release EVIOCGRAB, run `getevent -l` on the raw
  QEMU mouse node while injecting vertical and horizontal wheel events, then
  restart inputd and check its source capability/first-event logs. Run
  `getevent -l` on the virtual pointer to confirm 120-unit vertical and
  horizontal events, then verify list/browser content moves for wheel up/down
  and tilt left/right. Check whether QEMU's vmmouse/PS/2 AUX source reports
  wheel events; the USB tablet may have no wheel capability.
- The user's VM on port 5555 was not touched. No runtime scroll success is
  claimed until the fresh-image test is complete.
