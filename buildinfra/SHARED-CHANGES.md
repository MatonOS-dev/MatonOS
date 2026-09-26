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

- Launcher integration follow-up (2026-09-26): `apps/shelf/modules/matonos-shelf/android/src/main/java/org/matonos/shelf/ShelfService.java` still calls `ensureShellOverlayAccess()` and `prepareShellOverlay()` through its raw `ShellBridge` binding. Rewire those to the new typed `MatonosClient` methods so area apps don't call the bridge Binder directly. Its TS facade currently reduces task/navigation results to booleans or strings; preserve `{available, value, reason}` where callers need to report unavailable bridge features.
- Overlay permission mismatch to resolve before claiming Shelf-over-Settings works: `prepareShellOverlay()` checks `SYSTEM_APPLICATION_OVERLAY`, which this platform grants to the configured Recents component. The new configuration points that role at `org.matonos.recents`, while the persistent overlay window is owned by `org.matonos.shelf`. The Shelf package only asks for `SYSTEM_ALERT_WINDOW`; setting its app-op does not grant the system-overlay permission. Choose a design that keeps the exact-permission check meaningful and confirm the Shelf window can cross `HIDE_NON_SYSTEM_OVERLAY_WINDOWS` on a fresh image.
