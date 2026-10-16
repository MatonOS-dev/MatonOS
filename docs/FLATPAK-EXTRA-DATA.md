> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](RETIRE-LINUXD.md).

# Fixed-identity extra-data extraction

**Status (v3.5, 2026-10-16, branch `ostree-packs`): implemented as the
per-package service design at the end of this file** ("v3.5 design"). The
earlier "Stub design" sections are the superseded v3.4 approach, kept for the
policy analysis. Earlier sections are investigation only.

## v3.5 design (current)

Every generated package — app stub and runtime/extension package alike — gets
its own small service (`org.matonos.linux.stub.ExtraService`, AIDL
`IExtraExec`). The subsystem app installs the package's content (packs, no
applied extra output), then binds that service, and the service runs
`apply_extra` under the *package's own* UID. Vendor `apply_extra` code never
runs as the subsystem app.

Flow (requirements as shipped):

1. **Flatpak fork** (`common/flatpak-dir.c`, commit
   `4cce363ef3168baf10a4cd0a916694d3959f9558`): `apply_extra_data` is skipped at
   deploy time for **all** refs, not just apps. Downloads stay
   checksum-verified at pull time; the raw, still-unapplied files stay in the
   deployment's `files/extra` and are packed with the rest of the content.
2. **Per-package service.** `StubGenerator` declares `ExtraService` in every
   base (apps and `noActivities` runtime packages) with
   `org.matonos.linuxhost.EXTRA_MOUNT` naming the declared mount point
   (`/app` for apps, the runtime's own mount point otherwise). It mounts the
   package's packs read-only there through the v3 loader, copies its raw extra
   downloads from its own installed `pack_` carriers into a writable
   package-private directory, maps that over `<mount>/extra` (the map target is
   longer than the pack mount, so VFS longest-prefix lookup keeps it), and runs
   `<mount>/bin/apply_extra` with that directory as its working directory.
3. **Non-exported binding, enforced at call time.** A service in another app's
   package cannot be bound with `android:exported="false"` when the caller has
   a different UID, so the service is declared exported and refuses every
   caller whose UID is not `org.matonos.linux`'s
   (`Binder.getCallingUid()` checked on the binder thread, before the work is
   handed to a worker thread). This is the one deliberate deviation; it is
   recorded here and in the service's own documentation.
4. **Output by fd.** One pipe: a status byte, then for `STATUS_OK` package
   name, split name, versionCode, APK byte length, and the unsigned split APK.
   The subsystem app copies the bytes to staging after checking available
   space, validates only package/split/version identity from the binary
   manifest, signs with apksig and never unpacks or inspects payload entries.
   `STATUS_ERROR` carries a length-prefixed UTF-8 message; `STATUS_NONE` means
   there was no raw extra data.
5. **Publish as a split.** The package service uses `ObjectPacks` and
   `StubGenerator` to build a self-contained `extra_<sha256>` APK after
   `apply_extra`; the subsystem app signs it and installs it as a second
   `INHERIT_EXISTING` session (the package is not running yet). A timeout
   (service 14 min, client 15 min)
   or any failure fails the whole install; on a first install the package is
   removed, on an update the next update regenerates it.
6. **Launch.** `PackLaunch.argumentsWithExtra` mounts the base packs at the
   declared mount point and the `extra_` split at `<mount>/extra`, so the app
   and its runtime see exactly the tree `apply_extra` produced.
7. **Removed v3.4 path.** `ExtraData` (first-launch apply, `extra.state`, the
   private output directory, linuxd's `active` symlink) is deleted. Vendor
   apply_extra code never runs in the subsystem app.

Fidelity deviations from Flatpak's deploy-time step, all deliberate and
documented: no bubblewrap sandbox (the process is already in the package's own
UID and SELinux domain), the loader runs directly rather than through
`matonos-app-flatpak run`, and a `#!/bin/sh` shebang resolves through Android's
`/bin -> /system/bin` shell. Everything the proxy can forward unchanged
(scan/auth/EAPOL/data is a Wi-Fi concern; here, the whole `apply_extra`
execution environment) stays Android's own.

Launcher data comes from the deployment export, appstream and app id using the
ordinary subsystem path. The subsystem app never reads `extra/export` or any
other vendor-produced output. Consequently an `apply_extra`-generated desktop
entry cannot override the launcher metadata in v3.5; that behavior is dropped
to preserve the package-content boundary. The extra split contains the full
applied tree and remains independently mounted at `<mount>/extra`.

Host tests: `linux/stubgen/tests/run-tests.sh` plus the JVM unit tests
`LauncherDataTest` in the subsystem app. Device
checks are listed in `out/pc-logs/agents/extra-split.final.md`.

## Superseded: stub design (option 0 step 3), v3.4

Superseded on 2026-10-16 by the v3.5 per-package service design above. Kept for
the record: v3.4 ran an app's `apply_extra` on first launch inside the stub
process itself (`ExtraData.prepare`), which could not serve runtimes or
extensions and did not produce a durable, pack-format output.

Older investigation follows.

## Confirmed call path

`install/linuxd/FlatpakPublish.c:deploy_ref()` runs the wrapper's offline
`install --no-pull` as system. The wrapper clears its environment, including
any caller-supplied `FLATPAK_BWRAP`. Therefore setting that variable only in
linuxd would have no effect: the wrapper must explicitly select the helper
for authenticated offline deployment, after its existing root-path checks.
No new installation-path exception is necessary for this selection.

The checked-out Flatpak fork's `common/flatpak-dir.c:apply_extra_data()`:

1. Opens the runtime and app directory descriptors and binds them read-only
   at `/usr` and `/app`.
2. Binds the writable extra directory descriptor at `/app/extra`.
3. Sets that directory to 0700 immediately before spawning bubblewrap.
4. Runs `/app/bin/apply_extra` in its restricted sandbox, with no `/proc`.
5. Canonicalizes permissions before inspecting the child's exit status.

The directory does not exist early enough for a simple pre-deploy linuxd
chown. A helper can operate on the supplied FD, including reopening an
O_PATH descriptor. It would need to hand over existing downloaded files,
not only the containing directory. The helper must reject non-system UID
and non-linuxd domain callers before handling any FD or dropping identity.

`flatpak_canonicalize_permissions()` receives uid/gid -1 when the caller is
system, so it does **not** restore ownership. A supervisor must reclaim the
entire extra tree before returning to Flatpak, including failure paths.
Flatpak canonicalization is not an ownership rollback mechanism. The
supervisor must handle mode-000 directories, symlinks without following them,
malicious file types and extractor descendants before walking this tree.
The existing later publication/sanitization step is too late for this.

## Enforcing-policy blocker

The actual platform checkout's `system/sepolicy/private/domain.te`, around
line 2040, has a neverallow covering `no_x_file_perms` on `data_file_type`
for every domain except appdomain and the debug overlay remounter, with
exceptions for a short list of Android-owned code types. Our
`matonos_flatpak_deployment_file` and `matonos_linux_code_file` are
`data_file_type`, not those exceptions. `no_x_file_perms` includes execution
and executable mappings. A new native non-app extractor domain therefore
cannot execute the runtime shell or extraction tools from those deployments.

The historical data-execution exception in AGENTS.md is not present in this
platform checkout. Adding/reinstating a platform exception for a new domain
would change AOSP policy and needs authorization under the zero-patches rule.
No such exception was added. Assigning appdomain just to evade the neverallow
would import app privileges without establishing an app identity or a
reviewed security boundary; that is not an acceptable shortcut.

The existing store helper transitions to `matonos_flatpak_installer`, whose
neverallows explicitly prohibit writes to deployment, store and code labels.
Using that same domain for extraction breaks the staging/publication zone
boundary. Merely dropping UID leaves SELinux permissions unchanged. Running
vendor code in `matonos_linuxd` is likewise unacceptable even at UID 2902.

## Required design before implementation

If a future authorized design retains native bubblewrap, it needs these
scoped policy surfaces in the existing fixed policy files:

- A supervisor entry point callable only by linuxd, retaining ownership and
  relabel authority, with no vendor instructions in that process.
- A separate payload entry point/domain after setgroups(0), setresgid(2902)
  and setresuid(2902). No Binder, network, staging/store writes, Android app
  data or deployment/code writes. Enforce those exclusions with neverallows.
- A temporary extra-data label for this tree alone. Payload may mutate this
  label; runtime and app labels remain read-only. Supervisor alone relabels
  and reclaims the tree on success and failure. Restore original labels
  before normal deployment sanitization/sealing.
- Inherited-FD and capture-pipe use, only from the supervisor/linuxd; read and
  executable access to the pinned runtime/app; user-namespace setup and
  mounts on private tmpfs targets only. Derive precise setup permissions
  from the actual bubblewrap implementation and validate with selinux_policy.
- An explicitly authorized solution to the data-execution neverallow above.

A patch-free alternative would need an installer-identity worker using the
v3 in-process loader rather than kernel exec of deployment files. This is a
separate design: do not reuse an ordinary app UID, grant system identity to
vendor code, or claim the existing stub path already provides this worker.

The subsystem APK packages the helper executables beside the v3 loader. No
AOSP builds or VM mutations were performed for this documentation-only note.

## isolatedProcess option (option 0 step 3), 2026-10-16 — superseded

Superseded the same day by the stub design at the end of this file, which
runs `apply_extra` under the app stub's own UID instead of an isolated
process. Kept for the policy analysis.

Investigated moving only the `apply_extra` step into an
`android:isolatedProcess="true"` service in the subsystem app, with the
extra-data files and the app's file tree handed over as `ParcelFileDescriptor`s
and output written through a supplied directory FD. No runtime change is
shipped and no policy was added.

Result: **blocked by stock policy; stopped.** The loader that would run
`apply_extra` cannot reach the code it must run. Exact path:

- `system/sepolicy/private/app.te` (lines 806-823) is a hard platform
  `neverallow`: `isolated_app_all` may not hold `execute`/`execute_no_trans`
  (`no_x_file_perms`) on any `data_file_type` except `apex_art_data_file`,
  `dalvikcache_data_file`, `system_data_file` and `apk_data_file`.
  `matonos_flatpak_deployment_file` and `matonos_linux_code_file` are declared
  `file_type, data_file_type, core_data_file_type` in
  `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te`
  (lines 54 and 79), so the loader's `PROT_EXEC` mapping of `apply_extra` and
  the runtime is a neverallow violation. The type also covers
  `matonos_flatpak_staging_file` (line 78), i.e. the deployment at
  install time.
- `libmatonos-app-flatpak.so` is shipped in the subsystem APK native-library
  directory and executed through the v3 loader under the stub UID.
- Executing the loader itself is not the blocker: for the image host it is
  `system_file` (`system/sepolicy/private/file_contexts` line 545; copied in by
  `buildinfra/buildinfra.mk`), which `appdomain` may execute via
  `system/sepolicy/private/domain.te` line 164; for an updated host it is
  `apk_data_file`, allowed by `system/sepolicy/private/app.te` lines 470-471.
  `isolated_app` is in `appdomain`, so the loader starts but then cannot map
  what it was started to run.
- Passing FDs does not bypass SELinux: read/`PROT_EXEC` are re-checked against
  the file label and the isolated process's SID, and adding the missing allows
  would be a device-policy change (forbidden here) while the data-execution
  neverallow would still reject `execute`.

The subsystem app remains the reader of extra-data and the verifier/committer
of the pinned deployment; only the vendor `apply_extra` step was to move, and
it cannot run under `isolated_app` without an AOSP policy exception and a
device-policy grant. Both are out of scope under the zero-patches rule. No
service, AIDL, manifest or policy change was made; no image build or VM run.

Details and the stopped work item: `out/pc-logs/agents/apply-extra-isolated.final.md`.

## Stub design (option 0 step 3), implemented 2026-10-16

Shipped on branch `ostree-packs`. The installer never runs an
application's `apply_extra`; the generated stub applies it on first launch
and after every update.

A fetched commit's `xa.extra-data-sources` payload is checksum-verified at
pull time, so the raw, still-unapplied downloads are authenticated input.
They stay in the published deployment's `files/extra` (unsigned, but pinned
by the signed commit's metadata) and are packed with the rest of the
deployment:

1. Flatpak's `apply_extra_data` is skipped for `app/*` refs only, in the
   fork's `common/flatpak-dir.c:flatpak_dir_deploy` — the one local change
   to the fork. Runtimes and extensions keep upstream behaviour and are
   applied at install; only apps have a stub to defer to.
2. On launch, `ExtraData.prepare` reads linuxd's `active` symlink for the
   pinned installation, copies `files/extra` into the stub's private data
   directory (regular files and directories only; symlinks recreated but
   never followed), runs `/app/bin/apply_extra` there through the same v3
   loader the launch uses, and records the commit in `extra.state`.
3. `apply_extra` runs in the restricted `run --sandbox` form through
   `matonos-app-flatpak --command=/app/bin/apply_extra --cwd=/app/extra`, at
   the stub's own PackageManager UID and SELinux domain, with no bus, host
   filesystem or devices. The loader maps the private directory over
   `/app/extra`; that target is longer than the `/app` pack mount, so VFS
   longest-prefix lookup keeps it (verified against the loader's host
   scaffold).
4. The normal launch maps the same private directory over `/app/extra`, so
   the application sees exactly the tree its script produced.
5. A commit change (update) or a failed earlier attempt always wipes and
   regenerates the private output, so no stale tree survives. Uninstalling
   removes it with the app's data.

Why not the env-gated defer of the earlier plan: the wrapper
(`flatpak-env-wrapper.c`) calls `clearenv()` and rejects `run`, and
`matonos-app-flatpak` only selects the helper, so a variable set by linuxd
would be dropped; a per-process variable would also disable
runtime/extension extra-data application, which has no stub to fall back
on. Deferring by ref kind is the narrow, correct gate, and it matches the
live publication path: `FlatpakPublish.c:verify_staged_install` moves the
phase-C staging checkout into the installation and never re-deploys, so the
phase-C install is the only apply site.

Not reproduced: Flatpak's `extra/export` promotion (icons and desktop files
a script writes into `files/extra/export`), because the published
deployment is linuxd-owned and read-only. The raw downloads also stay inside
both the deployment and the stub APK packs — a space cost accepted for now
that is self-correcting once packs are built from the applied tree.

The device half of this change (the `MatonOS_apexs` rebuild of
`matonos-app-flatpak` with the fork change) is not buildable in this
worktree; the host code, its unit tests and the fork diff are in
`out/pc-logs/agents/apply-extra-stub.final.md`.
