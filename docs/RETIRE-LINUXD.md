# Retire linuxd — v3.5

Decision: delete the service, rather than reproduce it in another daemon. The
subsystem already produces signed pack APKs; PackageManager owns installed
packages, and the stub runs Flatpak under its own UID.

## Command and responsibility inventory

The deleted dispatcher accepted the commands below. `ILinuxd.call`, subscribe,
unsubscribe and its shell/root dumpsys snapshot were the complete transport.
No other daemon command is introduced.

| Deleted command/job | Caller before removal | Disposition |
| --- | --- | --- |
| begin_staged_install | LinuxAppStoreService.runStagedInstall | Minimal staging creation in InstallTransaction.prepare, under subsystem cache with a fresh internal UUID. No shared root, compositor UID argument or ownership handoff. |
| verify_staged_install | LinuxAppStoreService staged callback | Flatpak downloads using the pinned image Flathub remote/key; InstallTransaction runs OSTree fsck before building packs. DeployManifest checks requested origin and deployed commit identity. No untrusted producer crosses a UID boundary. |
| abort_staged_install | LinuxAppStoreService failure callbacks | InstallTransaction.cleanup removes only its private operation tree. No RPC. |
| bind_pending | LinuxAppStoreService.finishStubInstall | Deleted. The signed installed stub package is registration. |
| install_complete | LinuxAppStoreService.finishStubInstall | Deleted. PackageInstaller's success callback is completion. |
| install_failed | LinuxAppStoreService.result | Deleted. The store's existing operation callback reports failures and abandons sessions. |
| list_installed | LinuxAppStoreService.listInstalled | PackageManager inventory filtered through the existing sole-package/certificate/ref checks. |
| installed_commit | LinuxAppStoreService.remoteFor | Remote from signed stub metadata; original commit from signed index deployment.bin. No native query or separate state database. |
| uninstall | LinuxAppStoreService.uninstallStub | Existing PackageInstaller uninstall after authenticating the stub; PM removes private data/code. |
| reconcile_deployments | bridge LinuxdDeploymentReconciler boot/package/timer worker | Deleted along with the worker and service lifecycle hooks. There is no deployment store to reconcile. |
| subscribe/unsubscribe, progress snapshots/dumpsys | service listener registry; old install/tools test harness | Deleted. The store callback already supplies operation progress/results. The old daemon diagnostic harness is removed. |
| staging takeover, fs-verity sealing, sanitized publication | FlatpakManager/FlatpakPublish, reached by verify | Deleted. Trusted subsystem staging stays under its own UID; APK signatures/PM protect published code. No second installation tree. |
| app registration/origin pins, pending handoff | FlatpakManager, RemotePin, PendingDeployment | Deleted. Signed stub/index metadata and PackageManager replace the duplicate registry. Only the image's Flathub remote is supported. |
| deployment leases, retired-version GC | V3Launcher, DeploymentStore, FlatpakManager | Deleted. Installed APKs supply payloads; PM owns replacement and removal. |
| shared runtime publication/refcounts/GC | FlatpakManager, RuntimeRefs, FlatpakPublish | Deleted. Existing runtime APK sessions and package dependency inventory remain in LinuxAppStoreService. |
| launch lookup metadata | V3Launcher shared-install pointer | Minimal PackageDeployment/DeploymentRecord scaffolding in stub-private cache. Index split carries the original commit object, metadata and deploy bytes, hashed into its split name. No payload checkout. |
| helper environment, global HOME/cache, staging helper sync | flatpak-env-wrapper, linux-paths.h, stage-apex-helpers.sh | Deleted, including APEX module/payload and helper build entry. Existing unprivileged matonos-app-flatpak/bwrap entries remain. |
| init/storage/permissions | MatonosLinuxd.rc, install.mk, policy | Deleted; no boot service, CHOWN, system-owned /data/linux-app tree or Binder service label. |

Runtime APK installation, missing-runtime repair, per-package extra-data,
per-stub compositor/bus/DNS and PackageInstaller transactions existed before
this retirement; they are retained, not ports of daemon jobs.

## Wiring audited

- `install/linuxd/`: all native sources, headers, AIDL, Android.bp, init rc,
  tests and obsolete service-specific design documents removed.
- `install/install.mk`: package entry removed. Virtual A/B installer settings
  and snapuserd remain unrelated.
- `systembridge/aidl/.../ILinuxd.aidl`, host-local ILinuxdListener: removed.
  Bridge LinuxdDeploymentReconciler and SystemBridgeService start/close hooks:
  removed. ISystemBridge's public contract is unchanged.
- `LinuxAppStoreService`: every daemon call removed; private staging, PM query,
  metadata origin, package uninstall and callbacks remain in the subsystem.
- `V3Launcher`: shared-install paths, FileChannel/FileLock and pin/release code
  removed. PackageDeployment creates disposable metadata after runtime repair.
- `DeployManifest`, `StagedPacks`, `InstallTransaction`, install activity/service
  comments and local ICompositorInstall declaration: updated.
- `systembridge/sepolicy/system_ext/{public,private}`: domain/exec/tmpfs/service,
  code/deployment/staging types, all related rules and path/service mappings
  removed. `sepolicy/system_ext/private/matonos_controllers.te` contained only
  daemon grants; those are gone. Fixed policy files remain present.
- `linux/flatpak/Android.bp`: management wrapper prebuilt/module removed from
  APEX; sources, binary, stamp/provenance and binary license inventory removed.
  `tools/build-flatpak-helpers.sh` now builds only unprivileged applet entries.
- `install/tools/test-flatpak.sh` and `flatpak-operation-state.py`: removed;
  they depended on the dead service's dumpsys protocol.
- `rn-apps/flathub/.../MatonOSExpoModule.kt` and `ueventd.rc`: stale explanatory
  comments corrected; neither contains a daemon call.
- No dedicated linuxd AID exists: its rc used the shared system UID (1000).
  No active FPINSTALL/2902 declaration exists in this worktree.
- No vold change or independent shared-directory initializer remains in this
  tree. The sole live initializer was the removed rc. Old on-device stores
  are not migrated or recursively deleted by a new privileged component.

Keep `fsverity` in both fstab files. It is also used by stock
`frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java`:
`enableFsVerityToAddedApksWithIdsig` calls VerityUtils.setUpFsverity for v4 APK
signatures; ART-managed install files also use it. It is not linuxd-only.

## Verification

Run the subsystem-only build; restore the generated caller certificate
allowlist afterwards. Never commit generated APKs. Run stubgen/tests/run-tests.sh,
then stubgen/tests/deployment-flatpak-test.py with its compiled classes and an
output directory outside sources. The latter uses real OSTree commits and stock
host Flatpak to read installed commit/origin from package-derived scaffolding.
Run packs/tests/run.sh for three real commits, C reader, checkout reconstruction
and update replay. Existing StorePermissionPolicy, StubSelfInstallIdentity and
RuntimeMounts host JUnit tests cover authorization and extension mount decisions.

No image or VM was built here. Fresh enforcing-policy/image checks belong to the
coordinator. Old stubs/runtime APKs lacking deployment.bin need one reinstall or
update; launch fails explicitly rather than falling back to the old store.
Process-death transaction recovery remains a lifecycle check; the existing
operation registry is in memory. Interrupted private staging may remain in cache
until OS cache eviction; there is no privileged cleanup service.

## Coordinator device checks

Use a fresh image, disable VM idle suspend, and collect enforcing AVCs/logcat.

1. Install Firefox from the store. Check pack/index/runtime APKs are installed,
   the launcher entry appears, and no linuxd process, Binder service or newly
   created /data/linux-app directory exists. Launch, close and reopen; test
   Wayland/X11, graphics, sound, portals and host-command paths.
2. Update an app with a changed file and a removed file. Verify changed content,
   obsolete split removal, restart behavior and unchanged-pack reuse. A failed
   download or cancelled pre-commit install must leave the old package usable.
3. Uninstall through the store. Confirm PM removes the stub and its private
   data; unrelated apps keep working. Reinstall the same ref and launch again.
4. Remove a required runtime APK, then launch its stub. Confirm authenticated
   repair reinstalls runtime/extension/index packages and launch resumes. Also
   check wrong-signer runtime packages are rejected, not adopted.
5. Install/update Chrome's extra-data app. Confirm apply_extra runs only at
   its target package UID, output becomes an extra split, launcher/icon data is
   retained, and launch does not need the retired global store. Exercise a
   runtime extension with extra-data too.
6. Test offline/failing signature, corrupt staged content, cancellation,
   simultaneous callers reusing an external operation ID, process death during
   download/commit, and low disk space. Check bounded failure, no false success,
   no cross-operation deletion, and boot completion without hardware dependencies.

## Historical reference inventory

The following documents mentioned the old service when the audit started. They
are historical evidence, not instructions to restore a daemon. Current behavior
is defined above and in linux/packs/README.md and linux/subsystem/V3-LAUNCH.md.
- `HANDOVER-2026-10-02.md`
- `HANDOVER-2026-10-03-evening.md`
- `HANDOVER-2026-10-03-midday.md`
- `HANDOVER-2026-10-03.md`
- `HANDOVER-2026-10-04-afternoon.md`
- `HANDOVER-2026-10-04-evening.md`
- `HANDOVER-2026-10-05-late.md`
- `HANDOVER-2026-10-05-night.md`
- `HANDOVER-2026-10-07-afternoon.md`
- `HANDOVER-2026-10-10.md`
- `HANDOVER-2026-10-12.md`
- `HANDOVER-2026-10-13.md`
- `HANDOVER-2026-10-14.md`
- `docs/FLATPAK-EXTRA-DATA.md`
- `docs/FLATPAK-STUB-HANDOVER.md`
- `docs/HANDOVER-2026-10-07-per-app-runtime-sockets.md`
- `docs/HANDOVER-2026-10-08-flatpak-java-portal.md`
- `docs/LINUX-INSTALLER.md`
- `docs/LINUX-STUB-SELINUX.md`
- `docs/OWN-DBUS.md`
- `docs/PER-APP-RUNTIME.md`
- `docs/SOFTWARE-CENTRE.md`
- `install/README.md`
- `install/SHARED-CHANGES.md`
- `linux/README.md`
- `linux/compat-proxy/README.md`
- `linux/flatpak/APEX.md`
- `linux/packs/README.md`
- `linux/stubgen/README.md`
- `linux/subsystem/DNS-FORWARDER.md`
- `linux/subsystem/HOST-COMMAND.md`
- `linux/subsystem/V3-LAUNCH.md`
- `rn-apps/flathub/README.md`
- `rn-apps/flathub/SHARED-CHANGES.md`
- `sepolicy/README.md`
- `sepolicy/r24-enforcing-test-plan.md`
- `sleep/README.md`
- `NOTES.md` (coordinator-owned roadmap; update its historical linuxd entries when integrating)
- `HANDOFF.md` (historical boot/test notes)
- `AGENTS.md` (old storage-policy references; coordinator owns global instructions)

Validation in this worktree: subsystem APK build PASS; stubgen suite including
signed deployment-record carrier PASS; stock Flatpak lookup test PASS; real
OSTree pack reconstruction/update suite PASS; 10 store/identity/runtime-mount
JUnit tests PASS; repository preflight PASS. Preflight initially lacked 17
ignored inputs in the isolated worktree; existing APK/native prebuilts were
copied read-only from the main checkout into this worktree, then preflight was
rerun successfully. No other app or image was built. The generated signing
allowlist was restored, and no generated APK is part of these commits.
