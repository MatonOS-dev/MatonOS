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
