# Linux app stack — v3.5 repository split

MatonOS is based on AOSP. The subsystem APK sources, generated stubs,
OSTree packs, compatibility proxy/loaders, Flatpak, embedded PipeWire and
per-app session bus now live in the standalone repository:
https://github.com/MatonOS-dev/linux-subsystem-app

The device tree imports `prebuilt/linux-subsystem/MatonLinuxSubsystem.apk`.
Its `SOURCE` pins the source commit, APK hash and signer. The three executable
helpers beside it are extracted from that exact APK for the system copy's
nativeLibraryDir. The root `Android.bp` keeps the MatonLinuxSubsystem module,
partition and install path stable for StubLoader.

Build the APK and run its tests in the standalone repository with its root
`build.sh`; supply the existing signing keystore/password by external paths.
To update the image prebuilt:

1. Verify the new APK with SDK `apksigner verify --print-certs` and check that
   its signer is the expected subsystem key.
2. Copy it into `prebuilt/linux-subsystem/`, then run the standalone repo's
   `linux/compat-proxy/extract-loaders.sh APK OUTPUT_DIRECTORY` against it.
3. Update `SOURCE` with the exact source commit, SHA-256 values and signer.
4. Run `tools/build-apps.sh` (or its normal app selection) to regenerate bridge
   caller trust from the pinned APK. Other registered apps still build here.
5. Run `MATON_BUILD_COORDINATOR=1 tools/preflight.sh`, then request a fresh
   image build from the coordinator and test that image without hot-patching.

The system bridge, SELinux/image configuration and remaining image components
stay here. Xwayland, xkbcomp and their system libraries/data are preserved under
`prebuilt/xwayland/`; `tools/stage-xwayland.sh` refreshes that image component.

The source branch still uses `SessionBus` → `NativeBroker` → the native JNI
D-Bus broker. Java supplies desktop portals, not the session bus; the active
broker therefore moved with its tests. PipeWire is also packaged and used by
the APK. No kernel or SELinux changes are part of this extraction.

Runtime launch, storage and pack documentation moved with the standalone
source. An image build and fresh-boot/runtime checks remain integration work
for the coordinator; no runtime validation is implied by preflight or APK
compilation.
