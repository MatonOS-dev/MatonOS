# MatonOS input daemon

`matonos-inputd` is the input hardware proxy for generic x86_64 PCs. It is
built with the NDK outside Soong, starts from init in the shared
`matonos_driver` SELinux domain, and owns two persistent uinput devices named
**MatonOS Keyboard** and **MatonOS Pointer**. It discovers `/dev/input/event*`
nodes, grabs supported physical keyboards and pointer devices, and merges
their events into those stable virtual devices. Relative mice pass through;
multitouch touchpads become one-finger relative motion and tap/click events.
Absolute-only tablets are left for Android's input stack to handle.

Stock AOSP EventHub still enumerates readable physical evdev nodes after
another process has grabbed them; EVIOCGRAB makes those nodes inert to
InputReader but does not remove their names from dumpsys input or the input
device inventory. There is no per-device ignore setting in this branch. The
stable virtual devices carry the merged events, while hiding the physical
device inventory is dropped for zero patches; it would need a supported
EventHub exclusion hook or an AOSP patch.

Wheel events are merged from each source. Legacy `REL_WHEEL` and
`REL_HWHEEL` detents are converted to high-resolution units (120 units per
detent) when supported by the stable uinput pointer. If a source advertises
high-resolution axes and reports both forms in one frame, inputd uses the
high-resolution value; otherwise it falls back to legacy values at
`SYN_REPORT`. Two-finger scrolling and advanced touchpad gestures are not
translated yet.

The keyboard uses the complete AOSP Generic key layout and character map,
matched by its virtual vendor/product ID. Per-device keyboard layout selection
and hardware quirk tables are follow-up work; this first version forwards
supported Linux keycodes unchanged.

## Desktop mode and wake

Stock WM Shell enters desktop-first mode when it sees a keyboard and a
touchpad. When at least one real keyboard and pointer are connected, the
daemon creates a dormant virtual multitouch touchpad with
`INPUT_PROP_POINTER`; it destroys that device when either class disappears.
The virtual shim follows the stock EventHub touchpad classifier.

MatonOS keeps the display logically on; `matonos-sleepd` suspends the PC
after its idle timeout. The real PS/2 AUX wake source is enabled by
`power/pc-wakeup.sh`. Since inputd grabs the physical mouse, the merged virtual
event wakes InputReader and resets sleepd's idle timer after resume. The
virtual pointer IDC sets `device.wake = 1`.

QEMU uses q35's built-in PS/2 keyboard and a plain relative PS/2 mouse with
`vmport=off`. GTK grabs and confines the host pointer after a click in the VM
window; Ctrl+Alt+G releases it. The PS/2 mouse remains available for s2idle
wake. No absolute VM pointer, capture LED, display geometry IPC, or AOSP input
patch is used.

## Build and control

`native/matonos-inputd` is a source-tree symlink to `input/native`, allowing
the existing `tools/build-native.sh` loop to build this area without a tools
change. The daemon links the shared `libmatonos-ipc` helper. Its channel
exposes `get_state` and the `state` topic. The daemon publishes
keyboard/pointer presence as `vendor.maton.input.*` properties. A fixed import
stanza in `buildinfra/Android.bp` is requested in `SHARED-CHANGES.md`.

## Verification and open work

The prior inputd build and QEMU evidence exercised the retired VMMouse path and
do not verify this relative-only design. Verify a fresh image with the
instructions below. The no-hardware fallback remains non-blocking: the stable
uinput devices are created without requiring any physical input device.

### Fresh-image QEMU checks

1. Boot a newly built image with graphics and a serial log. Set
   `persist.vendor.maton.sleep_idle_s` to `0` before pointer checks.
2. Confirm inputd starts and check `getprop vendor.maton.input.*`.
3. Confirm MatonOS Keyboard and MatonOS Pointer remain present through
   physical hotplug. The grabbed physical devices may still appear in
   `dumpsys input`; verify their events do not reach Android.
4. In the QEMU window, click to grab the host pointer, then move/click/drag
   using the plain relative PS/2 mouse. Confirm Ctrl+Alt+G releases the host
   pointer, and keyboard input still works.
5. With a keyboard and mouse attached, verify the dormant MatonOS Desktop
   Touchpad appears; remove either device and verify the shim is removed.
6. Set a short sleep idle timeout and confirm PS/2 mouse movement wakes the
   suspended guest. Restore the property to `0` during the remaining checks.
7. Inspect logs for uinput, permission, AVC, input-reader, and daemon restart
   errors. Check the built image with `tools/check-selinux-labels.sh`.

### Real hardware checks

Install a fresh image on each machine; do not test by hot-patching system
files. Keep `persist.vendor.maton.sleep_idle_s=0` until input and resume checks
pass.

- **Build PC (Ryzen 5800X, RX 6600):** test the built-in and USB keyboards and
  mice, hotplug and replacement, META/Print Screen/media/brightness keys,
  desktop-mode shim creation, and mouse wake.
- **Surface Pro 3:** test the built-in keyboard/touchpad one-finger motion and
  tap/click, then attach a USB mouse and keyboard. Check desktop-first
  switching and platform input wake after a short suspend.
- **HP ProDesk 600 G1:** test PS/2 keyboard and mouse, USB hotplug and
  replacement, pointer movement/buttons, desktop-first with keyboard+mouse,
  and PS/2 AUX wake after suspend.

On all systems, verify a missing keyboard or pointer does not prevent boot and
the stable virtual devices remain registered through physical hotplug. Restore
normal idle timeout settings after wake tests.
