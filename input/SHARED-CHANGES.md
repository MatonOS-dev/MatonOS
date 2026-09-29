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

2. **Done:** the zero-patch migration retired these three files before the
   current build; they now live under `retired-patches/` and are not applied:

   - `patches/frameworks/native/0001-inputflinger-support-absolute-mice.patch`
   - `patches/frameworks/native/0002-inputflinger-PS2-mice-wake-the-display.patch`
   - `patches/frameworks/base/0002-WMShell-keyboard-and-mouse-start-on-the-desktop.patch`


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

# Conditional absolute-pointer mode (requested 2026-09-27)

The fresh QEMU boot showed that the absolute mode was not active:
`pointer_speed` remained 0, the acceleration setting was unset, and
`dumpsys activity services` showed no running bridge service. Although a
`BridgeBootReceiver` is already declared, ensure it starts the bridge and
keeps it running after `BOOT_COMPLETED`. The service should query
`input.get_state` and subscribe to `input/state`.
`matonos-inputd` now includes `absolutePointers` in both replies.
Apply the flat unit-gain mouse settings only while `absolutePointers > 0`; save
the active user's prior values and restore them when the count returns to
zero or the active user changes. Remove the unconditional settings writes from
bridge `onCreate`/`onStartCommand`. This keeps relative-only PCs' normal mouse
settings untouched while allowing the absolute-to-relative proxy to place the
cursor accurately. No AIDL or new channel instance is needed.

**Still pending:** the current shared `SystemBridgeService.java` calls
`setAbsolutePointerMode()` unconditionally from both `onCreate()` and
`onStartCommand()` and does not query/subscribe to the input channel state.
That global behavior can change settings on real PCs with only relative mice.
The bridge integration must be made conditional before this feature is
considered complete; this input area cannot edit the buildinfra-owned bridge.

# Fresh boot bridge failure (verified 2026-09-28)

The fresh image now installs and starts `org.matonos.systembridge`, and its
service successfully sends the 1600x900 display geometry to inputd. However,
both unconditional calls to `setAbsolutePointerMode(UserHandle.USER_CURRENT)`
fail with `SecurityException`: SettingsProvider rejects user `-2` from the
bridge app UID without `INTERACT_ACROSS_USERS_FULL`. Please use the concrete
active user ID (for the current single-user image, `UserHandle.myUserId()`)
when reading/writing pointer settings. At the same time, implement the
conditional state query/subscription requested above, and cache/restore each
user's settings on device removal or user change; do not change relative-only
PCs. Fresh-boot evidence: `out/pc-logs/input/final-bridge-errors.txt`.
