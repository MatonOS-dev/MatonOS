# Flatpak stub install handover

Date: 2026-10-06

## Agreed flow

- A store install creates a small signed Android stub first. The stub manifest
  carries the full Flatpak ref and remote; the ref provides the app ID,
  architecture, and branch. Flatpak resolves the runtime and dependencies.
- PackageManager assigns the stub UID. linuxd then stages and installs the
  Flatpak into the per-UID installation. Installed Flatpak data is keyed by
  that UID, with the existing runtime app UID used for the shared runtime
  installation.
- The store drives the install: it asks the bridge to create the stub
  (`install`), then starts the stub's unprivileged "install me" activity,
  which makes the bridge stage and install into the stub UID. The same
  activity resumes interrupted installs and is a no-op once installed. The
  stub never installs on launch. The stub's launcher activity ships disabled, so the app is
  hidden until linuxd reports a successful install, when the bridge enables
  it (the enabled override survives later stub replacements).
- Stub permission declarations are refreshed by the system after a successful
  install or update event. The stub does not request `regen` itself. The bridge
  compares the installed Flatpak metadata with the stub's requested Android
  permissions and replaces the stub when they differ.
- Stubs are new; backwards compatibility with earlier generated stubs is not
  required.

## Current implementation

The worktree changes implement the flow across:

- `systembridge/src/org/matonos/systembridge/FlatpakStubManager.java` — creates
  the initial (hidden) stub, deploys on "install me", and on the successful completion event enables the launcher activity and
  regenerates permissions.
- `systembridge/src/org/matonos/systembridge/SystemBridgeService.java` — comment
  update for the store `install` command.
- `linux/stubgen/src/org/matonos/linuxhost/stubgen/StubGenerator.java` — writes
  the Flatpak remote into generated stub metadata and emits the launcher
  activity with `android:enabled="false"` (attr `0x0101000e`).
- `StubGenerator` also emits the exported `InstallActivity`;
  `linux/compositor/.../stub/InstallActivity.java`, `IEmbeddedHost.installSelf`,
  `CompositorService`/`FlatpakLauncher`, `ISystemBridge.installFlatpakStub`,
  `SystemBridgeService` and `FlatpakStubManager.installSelf` implement
  "install me". Store: `rn-apps/flathub` module `startStubInstall` (Kotlin
  + `<queries>` in the module manifest), `FlatpakBridge.installApp` calls it,
  `resumeInstall(ref)` is exported for a future "resume" UI.
- `install/linuxd/FlatpakManager.c`, `.h` and `MatonosLinuxd.cpp` — the
  per-UID `installed_for_uid` check. (An earlier launch-time install path was
  removed after the store-driven decision.)
- `install/linuxd/README.md` and `linux/compositor/STUBS.md` — describe the
  updated contract.

The stub has no direct authority to call linuxd operations; "install me" is
idempotent and fully derived from the verified stub identity. The bridge uses
linuxd operations internally to check, stage, and install. Review the exposed
command allowlist separately if tightening the daemon API to only `install`,
`regen`, and `run` is still desired; the current internal API includes other
operations used by the store and lifecycle manager.

## Build and verification

The full coordinated build was started with:

```sh
out/pc-logs/agents/coord-build.sh --full -K -M -j 16
```

At handover it was still in the `build.sh` preflight check. The log stopped at
`######## Preflight`, and `preflight/checks.py` had spent several minutes
blocked on storage reads while walking the device tree. Check
`out/pc-logs/agents/build-status.txt` and `out/pc-logs/test-build.log` for its
latest state before starting another build. No successful build or runtime
verification of these changes has been recorded yet. Earlier `git diff
--check` passed before this build.

Builds must use `out/pc-logs/agents/coord-build.sh`; see `CLAUDE.md` for the
coordinator's Soong analysis and agent-freeze rules. Do not invoke `m` or
`lunch` directly.

## Next steps

1. Confirm whether the running preflight/build completed. If it is still
   blocked, identify which tree traversal is waiting on storage before
   restarting; do not run a competing build.
2. Review the complete diff, then run the coordinated build to completion and
   fix any compiler or Soong failures.
3. Exercise in a VM: first install (stub hidden until completion, then
   visible), successful Flatpak update, and permission-manifest replacement
   (launcher stays enabled after the stub is replaced), and "install me"
   after killing linuxd / the VM mid-install (plus a no-op call when
   installed).
4. Check failure/retry behavior for staging, PackageInstaller rejection, and
   lost or delayed linuxd completion events. Lost completion events are
   covered: the bridge's reconcile enables the launcher of every stub whose
   Flatpak is listed as installed.

## Interrupted installs (user, 2026-10-06)

A power cut or crash mid-install leaves a hidden stub with no (or a partial)
Flatpak. Recovery:

- "Install me": every stub has an always-enabled, unprivileged
  `org.matonos.compositor.stub.InstallActivity` (action
  `org.matonos.linuxhost.INSTALL`, `FLATPAK_REF` metadata, no launcher
  category, no permission). The store finds it by ref
  (`MatonOS.startStubInstall`, waiting up to 60 s for the new stub) and
  starts it after creating the stub, and again to resume. The bridge keeps the
  store's operationId from `install` so completion events still match. It reads the ref from the
  stub's own (disabled) launcher metadata and calls the compositor's
  `IEmbeddedHost.installSelf(ref)`; the compositor forwards the caller UID to
  the bridge's compositor-only `installFlatpakStub(uid, ref)`. The bridge
  verifies the stub, then asks linuxd `installed_for_uid`: already installed
  → just enables the launcher (nothing else); install running → nothing;
  otherwise it stages and installs from the stub manifest's ref and remote.
  The stub chooses nothing, so starting the activity needs no privilege.
- DNS (user, 2026-10-06; not wired yet): because the install starts in the
  stub's own process, "install me" can use the per-app DNS plumbing like a
  launch does: `InstallActivity` acquires the forwarder sockets
  (`createDnsForwarderSockets` → `DnsForwarder.acquire`) and passes its
  uid-derived 127.x endpoint, and staging/pulls resolve through it while the
  activity stays alive until the install completes.
- Cleanup (future linuxd): on boot, linuxd removes all dead stubs (hidden
  stubs whose install never completed).
5. Once verified, commit the Flatpak implementation together with this
   handover note.
