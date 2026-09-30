# Flatpak linuxd

`matonos-linuxd` exposes the Flatpak manager to the system bridge through the
existing `ILinuxd` Binder interface. It accepts structured JSON only, validates
refs and app IDs before invoking the fixed `/system_ext/bin/flatpak` binary,
and runs package operations on a worker while reporting progress. It starts
after `sys.boot_completed`; a missing service or Flatpak payload does not block
boot.

Run arguments are passed after `--`, and caller-provided values beginning with
`-` are rejected. Uninstall keeps app data unless `deleteData: true` is supplied.
Operations have a ten-minute CLI limit; listener callbacks run on a separate
bounded queue outside both operation and listener locks. Only one install or uninstall is accepted at a time. The store
tags each request with an operation ID and ignores completion events for any
other request. Access is checked with the bridge's
`org.matonos.permission.SYSTEM_BRIDGE` permission.

## Build and verification

- The service is compiled by the coordinating AOSP build; do not run `m` or
  `lunch` from this area.
- `linux/dbus-broker/build-android.sh` builds the standalone broker with the
  Android NDK into `out/pc-logs/dbus-broker/android`.
- Run `tools/preflight.sh` before requesting an image build.
- Runtime verification requires a freshly booted image: install, launch, and
  uninstall a harmless Flathub app; confirm option-looking run args are
  rejected, no-data uninstall preserves app data, progress/completion reach the
  store, and the system bridge is the only caller accepted by linuxd.

## Real hardware check

1. Boot the new image on the Ryzen/RX 6600/Intel 7265 build PC, Surface Pro 3
   (Marvell 88W8897), and HP ProDesk 600 G1. First boot each with networking
   disconnected and confirm Android reaches the launcher; this exercises the
   no-Flathub/no-operation fallback.
2. Connect networking, open Software Center, install a small app such as
   Calculator, and wait for its matching completion event. Confirm the busy
   state clears and the Installed list refreshes.
3. Launch the installed app, then uninstall it with the default choice and
   confirm app data is retained. Repeat with explicit `deleteData: true` and
   verify that data is removed.
4. From an authorized bridge test call, pass a run argument beginning with
   `--filesystem=` and confirm linuxd rejects it. Pass a normal positional
   argument and confirm Flatpak receives it after the `--` delimiter.
5. While an install is active, submit a second install and confirm linuxd
   rejects it promptly. Kill the linuxd process and confirm init restarts it
   without affecting boot or the rest of the running system.

## Open issues

- The bridge currently forwards Flatpak operations but does not generate or
  install a stub APK after an install. See [SHARED-CHANGES.md](SHARED-CHANGES.md)
  for the request to the separate bridge owner.
- The stub generator's AndroidKeyStore alias cannot survive app UID removal;
  stable bridge-owned stub signing is requested in `SHARED-CHANGES.md`.
- The bridge has a duplicate listener AIDL under `systembridge/**`. It remains
  synchronous here because that copy is outside this area's ownership. A
  stalled listener can stop event delivery, but does not block CLI draining or
  package operation serialization; listener death is tracked. Align both
  definitions as `oneway` in the bridge task.
- Full end-to-end install verification must wait for a successful shared image
  build and a fresh boot.
