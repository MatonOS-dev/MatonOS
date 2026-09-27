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

- Launcher findings above are superseded by the 2026-09-27 system-bridge nav-provider host: Shelf no longer starts at boot from its receiver; the bridge binds Shelf as the built-in, certificate-pinned default/fallback on first boot, with third-party services requiring explicit consent. The bridge uses `QUERY_ALL_PACKAGES` to verify arbitrary caller/provider certificates, and has no `<queries>` list. Shelf does not request `SYSTEM_APPLICATION_OVERLAY`; the platform-signed bridge owns a `TYPE_APPLICATION_OVERLAY` window with the system-overlay marker and the navigation inset provider. The shelf API no longer includes `prepareShellOverlay()`.
- Recents fresh-image observation (10:52 image): `smoke-split.sh` receives `Status: ok` for `.RecentsActivity`; logcat reports `Running "MatonRecents"` and bridge audit entries for `getRecentTasks` from the exact allowlisted Recents caller. The captured Recents image shows only the desktop wallpaper despite `dumpsys activity recents` containing live Settings, Shell and Recents tasks. Please diagnose the Recents JS/native data path or render root; no Recents-screen visual pass is claimed.

- Launcher compact Shelf / embedded-host synchronization: the typed navigation-provider APIs now include `INavigationBarHostCallback.onPreferredHeightChanged(int)` and `INavigationBarProvider.onHostSizeChanged(int, int)`. `NavigationBarProviderAdapter` exposes `reportPreferredHeight(int heightPx)`, marshals it and provider callbacks to the main thread, relayouts the existing `SurfaceControlViewHost` on size changes, and calls the protected `onHostSizeChanged(widthPx, heightPx)` hook without replacing the provider root. Shelf reports 28dp compact and 56dp expanded/home-visible using physical pixels. The bridge clamps dimensions and updates its host window + navigation inset. Keep this typed API in buildinfra/client; do not expose its Binder to app code.
