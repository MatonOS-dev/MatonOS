# Input shared changes requested

1. Add a fixed `cc_prebuilt_binary` import to `buildinfra/Android.bp` so the
   out-of-Soong build can stage and install the daemon:

   ```bp
   cc_prebuilt_binary {
       name: "matonos-inputd",
       srcs: ["native-built/matonos-inputd"],
       stem: "matonos-inputd",
       shared_libs: ["libmatonos-ipc", "liblog"],
       vendor: true,
   }
   ```

   `input/input.mk` already requests this module and copies the area-owned
   init rc, keyboard maps, and pointer IDC file. The existing
   `systembridge/res/raw/target_caller_allowlist.txt` already contains
   `input org.matonos.settings`; the daemon uses the existing generic bridge
   channel and needs no new bridge API.

2. After the first full-image build succeeds, retire these three patch files
   together, then run the next full build so `tools/apply-patches.sh` no
   longer applies them:

   - `patches/frameworks/native/0001-inputflinger-support-absolute-mice.patch`
   - `patches/frameworks/native/0002-inputflinger-PS2-mice-wake-the-display.patch`
   - `patches/frameworks/base/0002-WMShell-keyboard-and-mouse-start-on-the-desktop.patch`

   They remain in place until the replacement has passed a fresh-image boot
   and QEMU input checks.

3. Coordinator: list the strict stable-device-only Android inventory
   requirement under NOTES.md "Dropped for zero patches". Stock EventHub
   still enumerates readable grabbed evdev devices; this daemon can suppress
   their events and supply stable virtual event streams, but it cannot hide
   the raw device inventory without a framework exclusion hook.
# Absolute pointer acceleration and display geometry (requested 2026-09-25)

Implemented in `systembridge/SystemBridgeService.java`: inputd translates
absolute cursor positions into REL events, while the bridge disables mouse
acceleration and sets pointer speed to -7 (unit gain). The bridge applies this
on startup and exposes the same operation as `input.set_absolute_pointer_mode`
through the existing input target authorization path. The setting is global
because stock InputReader provides no per-device acceleration control. The
bridge package requests WRITE_SETTINGS and grants its own write-settings
AppOp using its existing app-op management permission.

The bridge sends `Display.getRealSize()` to inputd as
`call("input", "set_display_size", {"width": w, "height": h})` on startup
and every display change. This small edit is in the buildinfra-owned
`SystemBridgeService.java`; it uses the existing generic IChannel and retries
while inputd is not registered. A vendor property was rejected by Treble's
system-to-vendor property neverallow, so no display-size properties or policy
rules are needed.
