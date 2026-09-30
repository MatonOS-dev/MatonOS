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
- A minimal empty `resources.arsc` table required by Android PackageManager
  even though the stub declares no compiled resources.
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

## Stock PackageManager constraint

The requested provider layout cannot ship as one APK while the compositor
host contains its JNI libraries. AOSP's stock `PackageAbiHelperImpl` rejects
any package that declares a dynamic shared library and also contains native
libraries (`Shared library with native libs must be multiarch`). On the test
image the host APK was present under `/system_ext/app/MatonWaylandHost`, but
PackageManager did not register `org.matonos.compositor`; installing the
generated stub then failed with `INSTALL_FAILED_MISSING_SHARED_LIBRARY`.
This is a stock framework restriction, so this project will not patch it.

The stock-compatible follow-up is to split the Java dynamic-library provider
from the JNI-bearing compositor engine APK. Keep the provider package
`org.matonos.compositor` Java-only, including `StubActivity`, `StubService`,
`MainActivity`, `WindowActivity`, and `HostContract`; move the JNI libraries
and native compositor service into a companion package and connect the UI to
it through an explicit app-local bound service. Add the companion APK to the
shared app/build registries. Until that split is implemented and tested, a
generated stub can be signed and parsed, but it cannot be installed against
the host library or launched. See [SHARED-CHANGES.md](SHARED-CHANGES.md).

## Verification status

On a fresh QEMU boot, the latest generator produced a signed APK using
Android Keystore. The fixed manifest parsed through reconciliation; install
then failed because the host shared library was unavailable. The host APK
was present on `/system_ext`, but PackageManager did not register it. The
follow-up split documented above must land before PackageManager can resolve
the stub's `<uses-library>` and the launcher can run.

After the provider/engine split, repeat on a freshly built and booted image:

1. Expose `MatonLinuxStubGenerator` to a small trusted test caller (the store
   app or bridge; integration is out of scope here).
2. Generate a stub for `app/org.example.Test/x86_64/stable` with a minimal
   Desktop Entry, PNG, and no permissions.
3. Install it with `pm install -r /data/local/tmp/test.apk`, verify `pm path`
   and dynamic-library resolution, then launch its launcher activity.
4. Confirm the host shows the explicit “Flatpak runtime launcher is not
   wired” message until the runtime hook is connected.

## Follow-up contract for the bridge and store

The store must choose the package name, map requested Flatpak permissions to
the approved Android permission list, provide the exported `.desktop` file
and selected icon bytes, and invoke the generator when installing or
updating a ref. The bridge/store integration also needs a narrow caller
surface for this library and policy for which signed callers may mint stubs.
The installed per-device key must not be exported or copied; after a reset,
stubs are re-minted from installed refs and store metadata.
