# MatonOS Installer app

Expo SDK 57 / React Native 0.86 app using Expo UI Jetpack Compose. The app
owns install policy and emits a versioned, ordered list of generic service
operations from `src/installer/createV1Plan.ts`. The v1 16 GiB minimum, GPT
profile, 10.5 GiB super, 5 GiB per-slot groups, source mapping, data exclusions
and A-to-B sequence live here. The service must not encode these decisions.

The service API is one operation per request (`apiVersion: 1`, target disk ID,
typed operation); the app decides whether to submit the next operation. Its
mandatory safety checks re-enumerate immediately before each destructive
operation and reject the live source, mounted/in-use or read-only devices,
invalid extents and unsafe paths. The current client is a demo stub and never
writes storage.

User data transfer is file-level from the live decrypted user view into newly
initialized userdata. The app supplies visible-content paths and excludes
installer package data, package-manager records, private app state, FBE keys,
ADB secrets and other live-only state. It does not claim raw `/data` migration.

The installed-image finalization operation writes a dormancy marker for shared
first-boot integration to consume. Installed first boot disables the
privileged installer package for every user; live mode alone enables it. The
service remains disabled outside live mode.

See `../../install/PLAN.md` and `AB-LAYOUT.md` for the
build conversion order, disk contract and unresolved systemd-boot/XBOOTLDR
layout constraint.
