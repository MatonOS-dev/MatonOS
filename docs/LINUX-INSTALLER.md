# Compositor Flatpak installer

Design for the 2026-10-15 decision. This document specifies the replacement;
it does not mean all phases have landed. Phase B now provides linuxd registration,
verification and publication. Legacy inline install entry points fail closed
until the phase-C compositor/bridge flow is connected; UID-2902 helper packaging
remains for phase D. No migration or v2 repair is planned. Add no GPLv3 dependency or per-app exception.

## Ownership and protocol

The compositor (`org.matonos.linux`) performs Flatpak pull and deployment
at its PackageManager-assigned app UID. An application's `apply_extra` is
deliberately not run here: it is arbitrary vendor code, so the app's
generated stub applies it on launch (see
`docs/FLATPAK-EXTRA-DATA.md`). The compositor uses the image or
same-signer updated host's v3 loader, `matonos-app-flatpak` and
`matonos-app-bwrap`. It never runs as system, UID 2902, or linuxd's domain.
linuxd only authorizes, verifies, publishes and collects deployments. Only
linuxd mutates `/data/linux-app/runtime` and the install pointers.

The System Bridge owns the transaction and the final stub binding. Both
Software Centre install/update (`FlatpakStubManager.installStub`) and stub
self-update must enter this same flow. Neither entry point may retain an
inline linuxd download/deploy fallback.

Phase-B calls use the existing bridge Flatpak dispatch and private `ILinuxd`
transport. `begin_staged_install`, `verify_staged_install` and
`abort_staged_install` now exist on that private transport; the compositor
service and bridge orchestration below remain phase-C work:

1. Bridge authenticates the requesting store or signed stub using its existing
   capability checks, restrictions and per-device signing identity. It resolves
   the compositor UID through PackageManager, and registers a transaction with
   linuxd: `begin_staged_install {operationId, ref, remote, compositorUid}`.
   linuxd validates canonical ref/remote/operation syntax, pins the configured
   remote and its key, rejects duplicate active operations and creates staging.
   Do not accept a caller-supplied path, final app ID, signing key or keyring.
2. Bridge binds an explicit compositor installer service, authenticated in both
   directions. It passes the operation ID and authorized ref/remote. The
   compositor UID is derived from the service identity, not its manifest.
   No arbitrary shell arguments, loader flags or environment cross this API.
3. The compositor installs the complete transaction into its private staging
   installation, including runtimes and extensions. Runtime dependencies must
   be deployed here, never into the shared runtime store. It holds its own
   wake lock until the worker exits and keeps its own DNS forwarder alive.
   The host needs INTERNET plus the appropriate foreground-service permission
   and service declaration; the current host manifest has neither installer
   service nor INTERNET. `stub.InstallService` is only a progress service.
4. On successful worker exit, compositor calls the bridge's scoped completion
   entry point. Bridge forwards `verify_staged_install {operationId, manifest}`
   to linuxd. Failure calls `abort_staged_install {operationId}`. A generic
   trusted app must not be able to complete another compositor transaction.
5. linuxd resolves the registered transaction, obtains a stable private input
   as described below, verifies every selected deployment, and publishes an
   unpublished app version. It returns verified metadata/desktop/icon through
   the existing pending-deployment methods. No stub is generated from an
   unverified compositor manifest or deployment.
6. Existing bridge stub generation, PackageInstaller completion and
   `bind_pending` bind that verified version to the verified final stub UID.
   Existing update behavior keeps the running stub's leased version alive.
   `install_complete` is emitted after binding, not after network completion.

## Staging and manifest

All transport, Flatpak data, download scratch and extraction output stay in
device-encrypted storage on the install store's filesystem:

```
/data/linux-app/staging/<full-compositor-uid>/<operationId>/
    installation/        Flatpak installation, including repo/, app/, runtime/
    home/                installer HOME and XDG configuration
    cache/               Flatpak download cache
    tmp/                 TMPDIR
    run/                 private runtime and DNS-forwarder socket
```

The UID parent belongs to linuxd; the compositor owns its operation directory.
Other app UIDs cannot traverse it. Resolve directory components with pinned
FDs, O_NOFOLLOW and same-device checks. Do not use CE HOME/cache for staged
code, or relax validation to accept arbitrary app staging directories.

The completion manifest is a hint, with an exact schema and rejected unknown
fields: `version: 1`, `operationId`, `appRef`, `appCommit`, `remote`, and
`deployments: [{ref, commit, subpaths}]`. Refs identify their app/runtime kind,
architecture and branch; no filesystem paths are accepted. `subpaths` records
Flatpak partial extension deployments, not a permission to skip unchecked
files. Each commit is a lowercase 64-character SHA-256 value. Deployment
locations are derived from validated refs and Flatpak's authenticated commit
metadata. The manifest is passed as bounded channel JSON using the existing
channel framing limit, not executed or interpreted as command arguments.

linuxd re-derives the application ID from the authorized app ref, remote from
the registered transaction, deployed commit and runtime/extension relations
from verified OSTree metadata. It rejects mismatches and unexpected app refs.
Any recorded summary-verification setting/key from staging is untrusted.

## Verification and stable input

A signature authenticates OSTree objects, not independently writable deployed
files. `chown`, `chmod`, rename, a Java worker exit notification and advisory
locks do not revoke an existing writable FD or MAP_SHARED mapping. Relabeling
can deny subsequent write syscalls but must not be assumed to revoke an
already writable mapping. Killing the worker is insufficient when the
compositor itself is the untrusted output producer.

User decision (2026-10-15): seal every staged regular inode with fs-verity;
there is **no copy fallback**. linuxd first takes exclusive ownership of the
directory namespace, opens each regular file read-only, enables verity with
SHA-256/4096-byte blocks, and measures the seal. It authenticates signed bytes
through that sealed FD. The verity measurement is not the OSTree checksum:
OSTree must still check its own content and metadata hashes. An existing seal
is usable only after measuring and checking its hash algorithm and size.

Any sealing failure rejects the entire transaction and collects staging.
EOPNOTSUPP/ENOTTY reports "Cannot trust /data, expected fs feature 'fs-verity' isn't available"
(system integrity broken); ETXTBSY reports "staged file still open for writing" (producer retained
a writer or writable mapping). Other errors fail closed. Neither copying nor
an unsealed rename is an alternative. Directory entries and symlinks still
need the independent owned/rebuilt namespace boundary; verity does not cover
them. Never enable verity and then assume the directory tree is authenticated.

Phase B now implements registration and one active transaction, with explicit
staging/verifying/pending/failed states and bounded replay protection. linuxd
snapshots only the configured remote URL/key into its private trust repository
at registration; producer repo configuration, keys, refs and lock paths are
replaced before OSTree reads source objects. It rebuilds directories/symlinks,
counts every source hardlink alias, takes inode ownership, strips untrusted
xattrs and seals regular files before content verification. Aliases outside
the captured installation and aliases crossing app/runtime ownership zones
are rejected. Signed modes are preserved until exact comparison; unsigned
extra-data modes are canonicalized only in the authenticated designated zone.

A fresh trusted OSTree import checks real signatures/checksums and ref binding.
The verifier compares every selected deployment against a trusted checkout,
projects authenticated extension subpaths, validates runtime/extension metadata
relationships, confines signed absolute links and unsigned symlink chains,
and rejects special/setid files. It reconstructs Flatpak deploy GVariants,
repositories and active/current links from verified inputs; it never runs
Flatpak install/deploy/apply_extra. Runtime references are recorded before
shared publication. Existing runtime reuse requires verity state attested by
statx, signed-byte comparison and rebuilt-bookkeeping comparison; it adds no
verity ioctl permissions on live code labels. Verified new regular payloads
receive the established code labels before rename publication.

`StagedVerity.h` temporarily adds owner write permission when needed for
ENABLE_VERITY, whose FD remains read-only, then restores the original signed
mode on success and failure. This happens only after linuxd owns the inode and
namespace. It is not a writable content FD or a copying alternative.

The old inline prepare/install/deploy_pending/self-install entry points now
fail closed pending phase C. Existing pending metadata, lease and final bind
methods remain; binding checks the verified stub and same Android user.
Compositor worker cancellation/death handling and completion authentication
through the bridge still belong to phase C. UID-2902 removal and compositor
SELinux access remain phase D. Do not deploy B alone expecting normal installs.

Both userdata fstabs request `fsverity`. Live /data is ram0 formatted by stock
`system/fs/fs_mgr/fs_mgr_format.cpp`, not by make-live. Stock fs_mgr's ext4
mount precheck (`tune_verity` in `fs_mgr.cpp`) then runs `tune2fs -O verity`
before mounting. No AOSP formatter patch or make-live formatter exists here.
The narrow phase-B ioctl grant is linuxd → staging:file, commands 0x6685 and
0x6686 only; phase D still owns compositor access and the other policy zones.

`install/linuxd/tests/staged-handoff-race.py` now tests actual verity rejection
of retained writer FDs/mappings using the same per-file primitive. Unsupported
host filesystems return SKIP (77), or failure with `--require-verity`; they
never count as a passing regression. `MATON_VERITY_TEST_DIR` selects a prepared
verity-enabled filesystem. A standalone static initramfs regression (`staged-verity-vm.c`) also passes
against the existing MatonOS kernel and a newly formatted ext4 `-O verity`
disk, including a retained writable mapping with original mode 0444. It never
uses the user's Android VM. Syscall-contract tests use an explicit compile-time
mock; signed-tree/public-verifier tests use genuine OSTree signatures with
explicitly mocked verity ioctls/statx bits. The separate manager harness runs
real registry/pending/bind logic with mocked verifier calls. These tests do not
claim a complete enforcing Android install through the future phase-C flow.

After the private-input boundary:

1. Initialize a fresh trusted bare-user-only repository. Seed only the selected
   configured remote/key from linuxd's runtime store, never staging config.
   Use the existing `pull-local --untrusted --gpg-verify --remote=...` path
   backed by gpgme-lite/DullPGP. Verify object checksums as well as commit
   signatures. Do not trust a staging `rev-parse` result as authorization.
2. Resolve each commit in the trusted repository; check ref bindings and app
   metadata against the authorized request, including runtime and extension
   relationships. Verify all selected refs before touching shared pointers.
   Never run Flatpak install/deploy/apply_extra from linuxd.
3. Compare deployed signed files against the trusted OSTree dirtree/dirmeta:
   exact path set, type, signed mode, symlink text and content checksum.
   Handle explicitly selected extension subpaths using verified metadata.
   Check metadata and exported files too. Signed absolute runtime links need
   the existing confined-link treatment, applied only after authentication.
   Reject unexpected files; excluding the whole deployment because it contains
   extra-data would defeat verification.
4. Treat Flatpak-generated deployment bookkeeping separately: rebuild or
   validate each field against the trusted ref, commit and origin. In
   particular, do not copy untrusted `deploy` data, active/current symlinks,
   repository config, overrides or exports into the shared installation.
5. Only the verified commit's designated extra-data output is unsigned. Walk
   it FD-relative without following links; reject special files, cross-device
   traversal and setuid/setgid. Confine symlink resolution including chains,
   not just lexical `..` in one link. Canonicalize directories to 0755 and
   regular files to 0644/0755 according to their execute bit; strip untrusted
   xattrs/capabilities. Never carry hardlinks into another security zone.
6. Give verified app files the established deployment/code labels and final
   stub ownership through the existing pending/bind flow. Only linuxd merges
   verified runtime objects/deployments into the shared store. Persist runtime
   references before publication; finish the exact dependency set only after
   successful deployment. Preserve runtime hardlink sharing between trusted
   objects without allowing an app-owned staging inode to back trusted code.
7. fsync files and changed directories before committing existing atomic
   version pointers. The compositor never chooses a final stub UID or path.

## Failure, cancellation and cleanup

Keep the package-operation mutex for store mutations, not for the full network
transaction. Maintain operation state explicitly: authorized, staging,
verifying, pending-bind, complete/failed. Reject replay and concurrent
completion/abort. A failed verify never changes an existing install pointer.
Keep runtime reference protection for any shared objects already published,
as the current tracked-publish flow does on partial failure.

Cancellation stops/reaps the installer worker and descendants before deleting
scratch. Compositor service death fails staging, releases its wake lock and
DNS service, and lets linuxd collect the registered operation. Cleanup uses
FD-relative unlink without following attacker links; malformed or missing
state is empty, never a migration trigger. Startup collects abandoned staging
and private verification trees before accepting new mutations. Never remove
leased versions or a pointer that was committed before a final fsync error.

## Current file map and policy zones

| Phase | Files / responsibility |
| --- | --- |
| B | `install/linuxd/FlatpakPublish.{c,h}`: replace prepare/deploy with registration, private-input verification and publication; retain configured-key verification. |
| B | `install/linuxd/FlatpakManager.{c,h}`, `MatonosLinuxd.cpp`: route pending and self-install entry points; operation identity/state; no linuxd download/deploy fallback. |
| B | `install/linuxd/{PendingDeployment,DeploymentStore,RuntimeRefs,LinuxStorage}.h` and `RuntimeRefs.c`: retain leases, atomic binding, trusted runtime ownership and cleanup. |
| B | `install/linuxd/tests/`: real signed-commit fixtures and malicious deployed-tree tests; writable-FD/mapping, path substitution and hardlink tests. |
| C | `linux/subsystem/host/app/src/main/java/org/matonos/linux/`: installer service/worker in Java; share loader command construction and DNS lifecycle with `stub/V3Launcher.java` and `stub/DnsForwarder.java` without depending on `StubService.runtime()`. |
| C | `linux/subsystem/host/app/src/main/AndroidManifest.xml`: installer endpoint, network/foreground permissions. |
| C | `systembridge/src/org/matonos/systembridge/FlatpakStubManager.java` and bridge dispatch: authenticate/bind compositor, transaction completion and existing store/stub entry points. |
| D | `linux/flatpak/linux-paths.h`, `install/linuxd/FlatpakStore.c`, `linux/flatpak/flatpak-env-wrapper.c`, `config.fs` and helper packaging: remove fixed UID/stage only after the replacement works; keep unrelated helper commands. |
| D | `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te` and its existing public types/file contexts: grant only the compositor staging zone and linuxd verify/publish zone; coordinator validates `selinux_policy`. |

Required policy surfaces (not grants already added): compositor may traverse
the store root and write only its own staging operation, never shared runtime
or published code. Use a dedicated staging type and authenticated UID/MCS
ownership, without grants to all app domains over the shared store. linuxd may
read staging, create private verified code, relabel/chown verified output and
rename within store roots. Compositor-to-bridge completion uses existing
capability authentication; bridge-to-linuxd uses the existing private Binder transport.
Payloads keep the inherited unprivileged app domain/filter; add no native
extractor domain, Binder grant for Linux payloads, setuid helper or platform
neverallow exception. Any sealing ioctl needs a narrowly checked command and
permission on the staging type, not generic ioctl rights.

## Required validation

Host tests must use real OSTree signature/checksum behavior where available;
fakes must honor filtering and failure statuses. Exercise modified file,
mode/type/path, bad signature/key, unexpected app/runtime, malformed metadata,
symlink chain escape, FIFO/socket/device, hardlink alias, cancellation, stale
operation, writable mapping and source replacement. Preserve existing leases,
runtime references and concurrent users.

Coordinator rebuilds linuxd, bridge, host APK and policy after B-D. The
subsystem APK build packages the Flatpak tool inputs; build fork commit
`4cce363` in `~/matonos/repos/MatonOS_apexs` with `flatpak/build.sh` first.
On `adb -s localhost:5555`,
without stopping/rebooting the user's VM, verify Enforcing and install
Firefox (plain) and Chrome (extra-data) through the normal entry point.
Trace the installer pull/deploy UID and SELinux context: compositor's actual
app UID/domain, never 2902/system/linuxd. Then launch Chrome and trace the
`apply_extra` process: the stub's own app UID/domain (`untrusted_app_all`),
never the compositor, 2902, system or linuxd; confirm `/app/extra` resolves
to the stub's private directory. Check compositor DNS/wake-lock lifecycle,
shared-runtime ownership, signed file checksums, sanitized extra output, no
new relevant AVCs and successful launch. Exercise failing extraction,
cancellation, an update while an old leased version runs, another Android
user and missing state. A host regression probe alone is not a VM success
claim.
