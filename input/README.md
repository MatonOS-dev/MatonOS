# MatonOS input daemon

`matonos-inputd` is the input hardware proxy for generic x86_64 PCs. It is
built with the NDK outside Soong, starts from init in the shared
`matonos_driver` SELinux domain, and owns two persistent uinput devices named **MatonOS Keyboard** and
**MatonOS Pointer**. It discovers `/dev/input/event*` nodes, identifies
keyboard and pointer capabilities, grabs matching physical devices, and
merges their events into those two virtual devices. Virtual device nodes stay
registered when a USB keyboard or mouse is unplugged or replaced.

Stock AOSP EventHub still enumerates readable physical evdev nodes after
another process has grabbed them; EVIOCGRAB makes those nodes inert to
InputReader but does not remove their names from dumpsys input or the input
device inventory. There is no per-device ignore setting in this branch. The
stable virtual devices carry the merged events, while the stronger claim that
Android lists only those two devices is dropped for zero patches; it would
need a supported EventHub exclusion hook or an AOSP patch.

Relative mouse motion and supported keyboard keys pass through directly;
wheel events are normalized to the stable virtual pointer's reported
high-resolution axes as described below.
QEMU, VirtualBox, and VMware tablets report absolute coordinates and buttons
that stock InputReader does not classify as a cursor. For those devices the
daemon prefers the current Android display size, sent by the system bridge over the existing
IChannel `set_display_size` command at boot and on display changes; inputd
retains the latest dimensions in memory. It
rechecks these values each second, maps absolute coordinates to guest display
pixels, tracks the last target, and emits the difference as REL motion. If the
bridge channel has not provided dimensions yet, it checks the connected DRM connector's
current `mode`, then fb0's current virtual size; it never uses the advertised
mode list. It compensates for display density so InputReader's density scaling
does not change intended distance. The system bridge disables mouse
acceleration and sets pointer speed to -7 (unit gain), because stock AOSP has
no per-device acceleration control or absolute cursor mapper. This changes
the global mouse transform. The exact-corner test is pending a fresh image
containing this update.

Wheel events are merged from each source. Legacy `REL_WHEEL` and
`REL_HWHEEL` detents are converted to `REL_WHEEL_HI_RES` and
`REL_HWHEEL_HI_RES` units (120 units per detent) when supported by the stable
uinput pointer. If a source advertises high-resolution axes and reports both
forms in one frame, inputd uses the high-resolution value; if it reports only
the legacy value, inputd falls back to it at `SYN_REPORT`. This matches stock
InputReader's behavior of preferring high-resolution axes whenever declared.
Multitouch touchpads are reduced to one-finger relative motion; contact down/up
becomes a left-button press/release. Two-finger scrolling and advanced
touchpad gestures are not translated yet.

The keyboard uses the complete AOSP Generic key layout and character map,
matched by its virtual vendor/product ID. That map already maps Linux Super
keys to META and Print Screen to SYSRQ and includes standard media, volume,
and brightness keycodes. Per-device keyboard layout selection and hardware
quirk tables are follow-up work; this first version forwards supported Linux
keycodes unchanged.

## Desktop mode and wake

Stock WM Shell enters desktop-first mode when it sees a keyboard and a
touchpad. When at least one real keyboard and one real pointer are connected,
the daemon creates a dormant virtual multitouch touchpad with
`INPUT_PROP_POINTER`; it destroys that device when either class disappears.
The AOSP EventHub in this branch identifies a touchpad from multitouch X/Y
axes plus `INPUT_PROP_POINTER` when `BTN_TOOL_FINGER` is absent. The virtual
shim intentionally follows that stock classifier.

MatonOS keeps the display logically on; `matonos-sleepd` suspends the PC
directly after its idle timeout. The real PS/2 AUX wake source is enabled by
the existing `power/pc-wakeup.sh`. Since the daemon grabs the physical mouse,
the merged virtual event is what wakes InputReader and resets sleepd's idle
timer after resume. The virtual pointer's IDC sets `device.wake = 1`, so its
motion and buttons carry the normal input wake policy. The old inputflinger
PS/2 wake patch should therefore be unnecessary once this path is verified.

## Build and control

`native/matonos-inputd` is a source-tree symlink to `input/native`, allowing
the existing `tools/build-native.sh` loop to build this area without a tools
change. The daemon links the shared `libmatonos-ipc` helper. Its init socket
is `/dev/socket/matonos/input`; the generic System Bridge channel exposes
`get_state` and the `state` topic. The daemon publishes keyboard/pointer
presence as `vendor.maton.input.*` properties. A fixed import stanza in
`buildinfra/Android.bp` is requested in `SHARED-CHANGES.md`.

## Current verification and open work

- NDK x86_64/API 35 compile succeeded using the AOSP CMake/Ninja prebuilts.
- AOSP image build and fresh-boot tests are pending; the build request is in
  `out/pc-logs/agents/build-requests.txt`.
- The three existing AOSP patches remain in `patches/` until a fresh image
  proves the replacements. Do not delete them during an active build.
- QEMU absolute-axis mapping now uses DRM display geometry and position
  differences; NDK compilation passes. Fresh-image corner verification and
  the bridge operation that disables acceleration and sets unit pointer gain
  remain outstanding.
- Touchpad gestures beyond one-finger motion/tap and per-device key quirks or
  layouts are not implemented yet.
- AOSP continues to list grabbed physical evdev devices. Hiding them from its
  device inventory needs an EventHub exclusion hook, which is dropped for
  zero patches as described above.

## Fresh-image QEMU checks

Use the newly built image after the coordinator reports success. Run only one
VM at a time; do not remount or push binaries into a running image.

1. Boot headless with adb port 5567 and a serial log, using the command in
   `HANDOFF.md` and `tools/run-qemu-live.sh -g none -m 4096 -a 5567`.
2. Confirm the daemon starts: `adb -s 127.0.0.1:5567 logcat -d -s
   matonos-inputd`, and check `getprop vendor.maton.input.*`.
3. Run `adb shell dumpsys input` and confirm MatonOS Keyboard and MatonOS
   Pointer remain present. The physical grabbed devices may still be listed,
   but `get_state` through System Bridge should report the detected source
   keyboard/pointer counts.
4. With the default QEMU PS/2 keyboard + vmmouse, type into a text field,
   move the pointer, click, and drag-to-unlock. The VM should not require
   pointer capture. Physical keyboard/pointer devices may still appear in
   `dumpsys input`; verify their events do not reach Android while grabbed.
5. Add a QEMU USB tablet using the harness's extra-QEMU-args option. Move to
   each screen edge and click launcher items. Confirm Android reports a
   cursor-capable MatonOS Pointer, not a rotary encoder, and that movement and
   clicks work without pointer capture.
6. With a keyboard and mouse attached, verify the dormant MatonOS Desktop
   Touchpad is present; remove the mouse and verify the shim is removed.
   Repeat with the keyboard disconnected. Check WM Shell starts desktop-first
   on the full fresh boot with a keyboard and mouse.
7. Let the display remain logically on, set
   `persist.vendor.maton.sleep_idle_s` to a short test value, and verify a
   PS/2 mouse event wakes the suspended guest and reaches Android. Restore
   the property to `0` for the rest of testing.
8. Inspect `dmesg` and logcat for uinput, permission, AVC, input-reader, and
   daemon restart errors. `tools/check-selinux-labels.sh` must pass on the
   built image.

## Real hardware checks

Install a fresh image on each machine; do not test by hot-patching system
files. Keep `persist.vendor.maton.sleep_idle_s=0` until keyboard, pointer,
and resume behavior pass.

- **Build PC (Ryzen 5800X, RX 6600, Intel 7265 Wi-Fi/Bluetooth):** test the
  built-in keyboard/mouse, then USB keyboard and mouse hotplug. Confirm the
  MatonOS virtual keyboard and pointer IDs stay stable. Test META, Print
  Screen, volume, media, and brightness keys. Test the touchpad shim while a
  mouse is present, then remove the mouse and confirm the desktop-mode shim
  disappears. Set a short idle timeout and test mouse wake.
- **Surface Pro 3 (Marvell 88W8897 Wi-Fi/Bluetooth, Intel HDA):** test the
  built-in keyboard and touchpad with one-finger motion and tap/click, attach
  a USB mouse and keyboard, and verify desktop-first switching. Test PS/2 or
  platform keyboard/mouse wake after a short suspend; confirm the wake event
  reaches Android and sleepd's timer resets.
- **HP ProDesk 600 G1 (Haswell):** test PS/2 keyboard and mouse, then USB
  hotplug and replacement. Confirm pointer movement, left/right/middle clicks,
  META and Print Screen, desktop-first with keyboard+mouse, and PS/2 AUX wake
  after a short suspend.

On all systems, verify a missing keyboard or pointer does not prevent boot,
and verify the stable virtual devices remain registered through physical
hotplug. Restore normal idle timeout settings after wake tests.
