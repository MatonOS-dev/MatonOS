# Shared changes requested

- Update `CLAUDE.md` generic channel guidance to specify the frozen v1 stable
  `vendor.matonos.channel` VINTF AIDL service, NDK helper registration, and
  Binder listener callbacks. Remove the stale generic Unix-socket transport
  description while keeping unrelated audio PCM sockets documented separately.
- The channel policy adds the one fixed `sepolicy/matonos/service_contexts`
  mapping for `vendor.matonos.channel.IChannel/{sleep,wifi,bluetooth,input,audio,camera}`.
- The ODM bundle registry includes `bundle/matonos-channel.xml`; add later
  targets there and register them in the same VINTF package.

- Update `wifi/README.md`, `bluetooth/README.md`, and `input/README.md` to
  describe their `IChannel/<area>` service and remove the stale
  `/dev/socket/matonos/<area>` path. Their init rc socket declarations and
  symlink lines have already been removed as part of this transport change.

- Update `sepolicy/README.md` to remove its stale claim that the bridge connects
  to `/dev/socket/matonos/*`; describe the fixed `vendor_service_contexts`
  Binder channel mapping instead.
- Update `NOTES.md` bridge trust description: trust is enabled on all builds
  when Developer options are enabled; no persistent notification is required;
  the in-app request switch is off by default. Document `MatonOS.requestTrust`
  and REQUIRED/OPTIONAL library modes alongside the bridge design.
- Shell administration uses the bridge ContentProvider at
  `content://org.matonos.systembridge.shell`; only shell/root Binder UIDs may
  call it. No servicemanager service label or shell-facing SELinux rule is
  needed. The fixed `service_contexts` file remains for future platform
  services but has no MatonOS shell-command entry.

- Launcher follow-up after the 10:52 image boot (2026-09-26): Expo's `BootReceiver` repeatedly gets `BackgroundServiceStartNotAllowedException` when it calls `startService()` for `ShelfService` at boot / after `HOME_VISIBILITY`. Use an allowed background-start lifecycle (likely foreground service with the required type and notification, or another system-approved startup path) and verify the service stays alive after the receiver returns. The 10:52 APK no longer has the old bridge-thread `WindowManager.addView()` crash.
- Shelf-over-Settings remains blocked: on the fresh 10:52 image the `smoke-split.sh` script started Home, Drawer, Recents and Settings, then failed because the `MatonOS shelf` window was absent. `dumpsys package org.matonos.shelf` lists `SYSTEM_ALERT_WINDOW` and `SYSTEM_BRIDGE`, but Shelf does not request `SYSTEM_APPLICATION_OVERLAY`; the bridge's exact check consequently refuses `prepareShellOverlay()`. Add the special overlay permission request to the Shelf manifest and agree on a controlled way for this exact, built-in allowlisted app to receive it without handing another app the Recents role or bypassing WindowManager's permission check. The privileged bridge currently grants only the normal overlay app-op in `ensureShellOverlayAccess()`.
- Recents fresh-image observation (10:52 image): `smoke-split.sh` receives `Status: ok` for `.RecentsActivity`; logcat reports `Running "MatonRecents"` and bridge audit entries for `getRecentTasks` from the exact allowlisted Recents caller. The captured Recents image shows only the desktop wallpaper despite `dumpsys activity recents` containing live Settings, Shell and Recents tasks. Please diagnose the Recents JS/native data path or render root; no Recents-screen visual pass is claimed.
