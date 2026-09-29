# MatonOS Flatpak stub APK format v1

A stub APK is a small, signed package record for one installed Flatpak app. It
contains no application implementation. The shared code is supplied by the
updatable `org.matonos.compositor` host through Android's dynamic shared
library mechanism. The stub APK is signed on-device with the per-device key
held by Android Keystore; it is not portable to another device.

## Contents

The v1 package contains:

- A binary `AndroidManifest.xml` with the Flatpak display label, the required
  `<uses-library android:name="org.matonos.linuxhost"/>`, approved Android
  permission declarations, a launcher activity whose class is
  `org.matonos.compositor.stub.StubActivity`, and a placeholder service class
  `org.matonos.compositor.stub.StubService`.
- An `ACTION_MAIN` / `CATEGORY_LAUNCHER` filter. Each valid MIME type from the
  `.desktop` file's `MimeType=` entry is represented by its own `ACTION_VIEW`
  filter with `CATEGORY_DEFAULT` and `CATEGORY_BROWSABLE`.
- Activity metadata `org.matonos.linuxhost.FLATPAK_REF` containing the exact
  installed ref and `org.matonos.linuxhost.MIN_INTERFACE_VERSION` containing
  the minimum host interface version (currently `1`).
- The caller-provided PNG at `assets/matonos-stub/icon.png` and an empty,
  valid `classes.dex`. The icon is retained as a package asset; the store can
  decode/resize it when presenting the catalog.

Package names are selected by the caller and must be unique Android package
names. Stub format version is `1`; it is separate from the host interface
version. The Flatpak ref is checked against the standard
`app|runtime/name/arch/branch` form. Unsupported desktop MIME strings are
skipped. Localized desktop labels and desktop actions are not part of v1.

## Host interface

`HostContract.INTERFACE_VERSION` is the host ABI version. The host launcher
reads the ref and minimum version from the stub activity metadata. It routes
to the compositor host Activity and presents an explicit not-wired message
until the Flatpak runtime launch hook is connected. `StubService` is reserved
for future per-app D-Bus and portal integration; it does not implement those
features in v1.

The stub generator is `MatonLinuxStubGenerator` in `linux/stubgen`. Its Java
entry point accepts an app-private work directory, Flatpak ref, `.desktop` file, PNG bytes,
permission list, package name, and output APK file. It encodes the small
binary XML vocabulary directly, so it does not require AAPT2 on-device, then
signs with `apksig`. The Android Keystore alias
`matonos_flatpak_stub_v1` is created on first use as a non-exportable EC
P-256 signing key.

## Install and smoke test

After the coordinator builds and boots an image containing the compositor
host, expose `MatonLinuxStubGenerator` to a small trusted test caller (the
store app or the bridge; this task does not modify either). Generate an APK
for `app/org.example.Test/x86_64/stable`, with a minimal Desktop Entry file,
a PNG, and no permissions. Install it with `pm install -r /data/local/tmp/test.apk`.
Confirm `pm path` succeeds, PackageManager sees the required host library,
and launching the package's launcher activity opens Maton Wayland. Until the
runtime hook exists, the host screen should explain that the Flatpak runtime
launcher is not wired.

## Follow-up contract for the bridge and store

The store must choose the package name, map requested Flatpak permissions to
the approved Android permission list, provide the exported `.desktop` file
and selected icon bytes, and invoke the generator when installing or
updating a ref. The bridge/store integration also needs a narrow caller
surface for this library and policy for which signed callers may mint stubs.
The installed per-device key must not be exported or copied; after a reset,
stubs are re-minted from installed refs and store metadata.
