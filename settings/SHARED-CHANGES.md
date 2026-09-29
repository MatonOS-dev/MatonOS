# Settings integration requests

These are exact shared changes required before Settings is buildable and its
entry points/features are verified. Shared edits belong to the coordinator or
the owning agent.

## App registry and import

1. DONE in the working tree: the `matonos-settings` row in
   `buildinfra/apps/apps.list` now points to `../../../apps/settings` in Expo
   mode, retaining key id `matonos-settings` and output `Settings.apk`. The
   fixed Soong module `MatonOSSettings` and package-scoped privilege XML/import
   remain. The Java project is parked until the Expo APK is built and staged.
2. Retire the Java project after the new Expo APK is staged and validated.
   Do not delete its source during an active build.
3. Refresh `systembridge/res/raw/caller_cert_allowlist.txt` from the existing
   `matonos-settings` key after the Expo APK build. Keep existing exact
   `status`, `input`, `sleep`, `wifi`, and `bluetooth` package-target grants.
4. Add exact bridge authorization row `install org.matonos.settings` in
   `systembridge/res/raw/target_caller_allowlist.txt` only once the install
   service is registered and independently enforces live-only safety.
   Reason: Settings is the sole UI for the installer; the bridge currently
   has no `install` target grant for this package.

## Bridge and daemon APIs

1. Add a public app-facing bridge method `isLiveImage(): boolean` backed by
   the bridge's internal read of `ro.boot.matonos.live`, and expose it through
   `buildinfra/client`/MatonosClient. Replace Settings' temporary public
   `getprop` subprocess check in the boot receiver and live-section gate.
   Reason: app code must not use hidden SystemProperties APIs and should not
   spawn a shell utility at boot.
2. Extend sleepd's existing `sleep` channel with `set_idle_timeout` (bounded
   integer `seconds`) and `set_mode` (documented enum); publish the updated
   state. Current source only registers `get_state`, so Settings controls
   cannot currently apply changes.
3. Complete the `install` channel backend and bundle integration before
   enabling disk writes: `get_status` (including `executorReady`, `dryRunVerified` and
   `readbackVerified`), `list_drives`, `execute_operation`, scoped progress,
   cancellation between safe operation boundaries and the typed API v1
   payload. The existing C++ code validates generic requests but has no block
   device executor or registered Binder channel. Run dry-run plus bounded
   readback/verification against an explicitly disposable QEMU target disk.
   Preserve fail-closed live-image, live-medium, mount/use/read-only,
   re-enumeration, path and extent checks. A/B conversion remains separately
   out of scope.
4. Expose an app-facing bridge method returning the specific `vendor.maton.*`
   hardware summary, or typed read-only channel state from the owning daemons.
   HardwareModule now reports Wi-Fi, Bluetooth, audio and GPU as Unknown; it
   no longer reflects into `android.os.SystemProperties`. Reason: private
   framework APIs are reserved for the system bridge. Keep state read-only in
   Settings v1.

## Build and verification

After the app registry is switched and `build-status.txt` is not RUNNING,
request a coordinated image build. On the fresh image, verify the dynamic
Settings homepage tile appears, Settings has no LAUNCHER component, the live
alias is enabled only with the live marker, and Android Settings/boot complete
normally when optional channels are absent.
