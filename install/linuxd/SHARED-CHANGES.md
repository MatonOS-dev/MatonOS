# Shared Flatpak integration

The repair now connects the bridge to `MatonLinuxStubGenerator`, imports
validated desktop metadata through linuxd, installs generated APKs through
PackageInstaller, and reconciles removed apps. Signing is scoped to the bridge
UID. Both copies of `ILinuxdListener` now use `oneway` callbacks.

The compositor now delegates its Wayland socket directory through the bridge;
linuxd relays Unix streams and SCM_RIGHTS descriptors to launch graphical
Flatpaks. Exported PNG icons become real Android drawable resources. Recovery
after bridge data/key loss remains pending.
Build and runtime status is recorded in the coordinator handoff.

## linux-data, 2026-10-04 (narrowed scope)

- No stub label, PackageManager dataDir or new socket API is needed. The earlier
  custom-stub-label exception request is withdrawn.
- Coordinator: record the already approved 0001-data-exec-exempt-domain.patch
  in preflight/checks.py APPROVED_PATCHES and stage the 19 missing APK imports.
- Rebuild APEX, linuxd, bridge, SELinux and image together; test only a freshly
  booted image. No kernel changes or new policy files are needed.
- Stock policy has unconditional minimal Binder grants to all domains. Existing
  neverallows prohibit additional rights; the sandbox seccomp filter rejects
  all Binder ioctls. Full SELinux subtraction would need another AOSP change,
  which this task does not introduce.
