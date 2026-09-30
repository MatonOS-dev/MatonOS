# Shared changes requested

- **System bridge Flatpak stub lifecycle (separate bridge task):** after an
  authorized Flatpak install, enumerate the exported desktop entry and icon,
  invoke the stub generator, and install/register the resulting APK; remove the
  generated stub on uninstall. `ILinuxd` currently exposes only Flatpak CLI
  operations and cannot make an installed Linux app launchable from Android.
  Keep the existing authorization checks around install/uninstall and validate
  the ref before passing it to the generator.
- **Stable stub signing identity:** AndroidKeyStore aliases are scoped to the
  app UID and are removed on uninstall/data clear. Please provide a bridge-owned
  signing key or signing operation for generated stubs so a reinstall does not
  change the package signing identity. The current generator cannot safely
  preserve an alias across app removal on its own.
- **Nonblocking linuxd listener contract:** `ILinuxdListener.aidl` is duplicated
  under `systembridge/aidl`. linuxd currently dispatches callbacks on a bounded
  worker queue so a stalled bridge cannot block install execution. Please
  change both AIDL definitions to `oneway interface ILinuxdListener` together;
  changing only the linuxd copy would make the generated Binder stubs disagree.
