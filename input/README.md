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
does not change intended distance. InputReader also applies its cursor density
scale (`viewportDensity / 320`) and the normal mouse velocity curve. Exact
absolute placement therefore needs that curve flattened while an absolute
pointer source is present. The bridge's existing setter was not active in the
first runtime check: `pointer_speed` stayed at 0, acceleration was unset, and
the bridge service was not running. A channel state field and a conditional
bridge update are now requested in `SHARED-CHANGES.md`; relative-only devices
must keep the user's normal pointer settings.

In this AOSP branch, EventHub classifies a cursor only when the device has
`BTN_MOUSE`, `REL_X`, and `REL_Y`. Its touch-device path requires multitouch
position axes, or `BTN_TOUCH` plus `ABS_X`/`ABS_Y`; `touch.deviceType=pointer`
only changes how that already-classified touch device is mapped. Therefore an
absolute mouse that reports buttons plus `ABS_X`/`ABS_Y` is not made into a
cursor by an IDC alone. The proxy converts its absolute target positions into
relative deltas on the stable virtual pointer, while physical relative devices
continue through the existing relative-event path.

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

- `tools/build-native.sh` compiled the updated x86_64/API 35 daemon, and
  `tools/preflight.sh` passed on 2026-09-27.
- Fresh QEMU boot on the 2026-09-27 20:38 image reached
  `sys.boot_completed=1`. The kernel has `CONFIG_MOUSE_PS2_VMMOUSE=y` and
  loaded `psmouse`. With the existing `q35,vmport=on` harness (no explicit
  `-device vmmouse`), QEMU exposed `VirtualPS/2 VMware VMMouse` as two evdev
  nodes: one BTN_MOUSE + ABS_X/ABS_Y (0..65535), and one BTN_MOUSE + REL_X/REL_Y.
  QEMU's monitor lists `vmmouse (absolute)` as the active mouse.
- `matonos-inputd` grabbed both VMware nodes. Stock EventHub does not expose
  the grabbed ABS node to InputReader; it sees the REL node as a cursor and
  also sees the stable `MatonOS Pointer` as a cursor. `dumpsys input` showed
  the stable pointer loads `/vendor/usr/idc/Vendor_4d54_Product_0002.idc`.
  `touch.deviceType=pointer` cannot classify the raw ABS-only mouse because
  EventHub never assigns it the touch class needed to create a touch mapper.
- A separate fresh boot of the same image with `-device usb-tablet,bus=xhci.0`
  exposed `QEMU QEMU USB Tablet` with ABS_X/Y (0..32767), BTN_MOUSE, and
  relative wheel events. Stock InputReader classified it as
  `ROTARY_ENCODER | EXTERNAL` with source `ROTARY_ENCODER`, while the PS/2
  VMMouse remained the usable cursor. With the guest asleep, selecting/moving
  the USB tablet did not wake it. The USB controller's sysfs wakeup flag read
  `enabled`, but this QEMU xHCI route did not wake s2idle. A direct QMP absolute
  event on the active VMMouse did wake the guest (`mWakefulness` changed from
  Asleep to Awake). This rules out usb-tablet for the wake requirement.
- Fresh 22:38 image: QMP absolute coordinates are normalized to 0..32767;
  QEMU expands them to VMMouse evdev's 0..65535 range. With unit-gain
  InputReader settings in this disposable VM, QMP points 0, 16384, and 32767
  landed at (0,0), (800,450), and (1599,899) on a 1600x900 display. The pointer
  is visible in `fresh-center-normalized.png`; the matching `MotionEvent`
  coordinates are in the adjacent `fresh-*-normalized.txt` files under
  `/mnt/data/aosp/out/pc-logs/input/`. This confirms host-normalized position
  and Android cursor position stay aligned without pointer capture when the
  pointer transform is flat and unit gain.
- Fresh-image s2idle wake: `/sys/power/mem_sleep` reported `[s2idle]`. A root
  shell wrote `mem` to `/sys/power/state`; its command remained blocked until a
  QMP relative PS/2 mouse event arrived. Guest `dmesg` recorded `PM: suspend
  entry (s2idle)` at 1347.081 s and `PM: suspend exit` at 1394.905 s. The QEMU
  monitor listed only the PS/2 mouse during suspend; after resume it restored
  `vmmouse (absolute)`. Evidence is in `s2idle-wake-kmsg.txt` and
  `fresh-vmmouse-wake.txt` under the same log directory. The idle property was
  reset to 0 before the VM was shut down.
- The VM entered the configured sleep path before I disabled its idle timer;
  the next coordinator build then stopped it. I did not obtain a usable
  on-screen cursor screenshot or complete the edge/re-entry checks. The
  coordinator stopped the tablet comparison VM while I was collecting the
  post-wake capture.
  Logs and captures from that boot are in `/mnt/data/aosp/out/pc-logs/input/`.
- Absolute-axis deltas now invert InputReader's `viewportDensity / 320` scale
  directly. `absolutePointers` is included in the input channel state so the
  bridge can enable its flat, unit-gain transform only while an absolute
  pointer is connected, then restore the user's settings. Native rebuild and
  preflight succeeded. The 22:38 image reached `sys.boot_completed=1`. On
  QEMU's normalized QMP range (0..32767), target points 0, 16384, and 32767
  produced InputReader coordinates (0,0), (800,450), and (1599,899) on the
  1600x900 display. `fresh-center-normalized.png` and
  `fresh-top-left-normalized.png` show the cursor at the center and upper-left
  corner; matching `dumpsys input` motion records are in the adjacent
  `fresh-*-normalized.txt` files under `/mnt/data/aosp/out/pc-logs/input/`.
  This placement check used temporary QEMU-only settings
  `pointer_speed=-7` and `mouse_pointer_acceleration_enabled=0`, because
  SystemBridge was not running in the image. Default settings on that boot
  were speed 0 and acceleration unset, so automatic enable/restore behavior is
  still unverified. The current bridge source still writes the global settings
  unconditionally on service startup; see `SHARED-CHANGES.md`.
- Touchpad gestures beyond one-finger motion/tap and per-device key quirks or
  layouts are not implemented yet.
- AOSP continues to list grabbed physical evdev devices. Hiding them from its
  device inventory needs an EventHub exclusion hook, which is dropped for
  zero patches as described above.

## Fresh-image QEMU checks

Use the newly built image after the coordinator reports success. Run only one
VM at a time; do not remount or push binaries into a running image.

1. Boot with graphics enabled on adb port 5567 and a serial log:
   `tools/run-qemu-live.sh -g virgl -m 4096 -a 5567 -s
   /mnt/data/aosp/out/pc-logs/input/serial.log`. After adb connects, run
   `adb root` and immediately set `persist.vendor.maton.sleep_idle_s` to `0`.
   This prevents sleepd from suspending the guest during pointer checks.
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
5. Do not use `usb-tablet` for the supported path: the fresh boot classified
   it as a rotary encoder, and its events did not wake s2idle. Keep QEMU's
   built-in PS/2 controller plus `vmport=on` VMMouse. `virtio-tablet-pci` is
   an untested alternative; only consider it after a separate fresh boot
   confirms stock cursor classification and PS/2 s2idle wake still works.
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

## Fresh verification after SystemBridge install fix — 2026-09-28

On the coordinator's fresh full image, boot completed (`sys.boot_completed=1`),
`matonos-inputd` ran, and `org.matonos.systembridge/.SystemBridgeService` was
installed and started. `getevent -lp` showed the expected QEMU VMware VMMouse
pair: one PS/2 AUX REL_X/REL_Y mouse for wake and one ABS_X/ABS_Y device with
range 0..65535. `CONFIG_MOUSE_PS2_VMMOUSE` is enabled. InputReader classified
the inputd virtual device as `Classes: CURSOR`, `Sources: MOUSE`, with a
1600x900 display range. The bridge successfully delivered display geometry.

The bridge's absolute-pointer setting call failed on boot. It passes
`UserHandle.USER_CURRENT` (-2) to SettingsProvider, which rejects the bridge
app UID without cross-user permission. Defaults remained pointer speed 0 and
acceleration unset. In this disposable QEMU user, setting speed to -7 and
acceleration to 0 allowed a clean placement check; no AOSP or kernel change
was made. QMP absolute values 0, 16384, and 32767 produced InputReader cursor
positions (0,0), (800,450), and (1599,899), respectively, on the 1600x900
display. The center screencap `out/pc-logs/input/final-center.png` visibly
shows the Android cursor at center. The bridge fix and conditional settings
restore are requested in `SHARED-CHANGES.md`; until that lands, this mode is
not automatically enabled and relative-only host settings are not yet proven
to be preserved by the bridge.

Mouse wake also passed on this boot. `/sys/power/mem_sleep` reported
`[s2idle]`; `echo mem > /sys/power/state` entered suspend, and a QMP REL event
on the remaining PS/2 mouse woke it. Guest dmesg recorded suspend entry at
199.166528 s and exit at 206.344752 s; boot remained complete afterward. The
idle timer property was restored to 0 before shutting down the 4 GB VM.

Runtime evidence is in `out/pc-logs/input/final-bridge-errors.txt` and
`out/pc-logs/input/final-center.png`. Host cursor rendering was not separately
captured; QMP absolute input coordinates and Android cursor coordinates match.
