# Installer area

The design decisions and partition contract are in [PLAN.md](PLAN.md) and
[AB-LAYOUT.md](AB-LAYOUT.md). `service/InstallerService.*` is the NDK-safe
drive-enumeration, generic operation request types, per-operation validation
and fresh-snapshot dispatch boundary. It does not write block devices or
register the channel yet. Layout and sequencing policy live in
`apps/installer/src/installer/createV1Plan.ts`, not in service validation.
The typed Expo client is currently backed by a demo stub, so the UI never
modifies storage.

## Build and local checks

- `./install/build-core.sh` compiles the service core with NDK r30,
  x86_64/API 35, at two jobs.
- In `apps/installer`, run `npm ci`, `npm run typecheck`, `npm run lint`, and
  `npx expo prebuild --platform android --clean --no-install`.
- The area has not been integrated into `tools/build-native.sh`,
  `tools/build-apps.sh`, the ODM bundle, the bridge allowlists, or Soong
  imports. Those exact shared changes are listed in
  [SHARED-CHANGES.md](SHARED-CHANGES.md). No AOSP build or VM was requested
  for this low-priority work.

## Before enabling installation

1. Convert the base build to A/B and make the live image use only `_a` while
   retaining compact exact-fit `super`; request a coordinated full build and
   fresh live boot after each layout change.
2. Resolve the documented systemd-boot constraint. The contract currently
   uses one XBOOTLDR with per-slot boot directories; physical `boot_a` and
   `boot_b` require a demonstrated compatible boot path.
3. Integrate the daemon over `vendor.matonos.channel.IChannel/install`, add
   its `matonos_installer` SELinux domain to the fixed policy files, and
   register it in the ODM bundle. Add the app import, own signing key,
   package-scoped bridge permission allowlist and exact caller certificate.
4. Replace the stub client with the bridge channel adapter; generate separate
   live/installed first-boot behavior that enables the installer only in the
   live session and leaves the installed system app disabled for every user.
5. Implement a resumable decrypted-file export/import. Do not copy raw live
   `/data`, FBE keys, ADB private keys, installer app data, or package-manager
   records. Private app data is supported only through app backup/restore.
6. Verify destructive safety checks, target-only boot, both identical slots,
   installer dormancy, successful fallback, and `/data` persistence before
   calling the installer complete.

The app-authored v1 profile and generic operation wire contract are documented
in [AB-LAYOUT.md](AB-LAYOUT.md). Service safety is mandatory for every
operation; v3 should add app policy using existing primitives, not a
service-owned installation workflow.

## Later hardware test matrix

- Ryzen 5800X / RX 6600 / Intel 7265 Wi-Fi+BT build PC: list all attached
  whole disks, show correct model/size, keep the live disk and mounted disks
  disabled, install to a blank target, boot target alone, and verify both
  slots plus restored shared files.
- Surface Pro 3: verify internal storage enumeration and refusal of the
  current live medium; install to an external blank SSD, then verify reboot
  and user-file restore.
- HP ProDesk 600 G1 Haswell: verify the installer and service start correctly
  with optional Wi-Fi/Bluetooth hardware absent, and verify an interrupted
  target write does not prevent another live boot.
- Any PC with no eligible target: UI must show the reason per disk and must
  not block or delay normal live-system boot.
