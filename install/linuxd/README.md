# Flatpak management service

linuxd is a late-start system_ext service. Its absence never blocks boot.
The authenticated `ILinuxd` interface serves install/update/store configuration,
metadata, icons, desktop entries, inventories, reconciliation and stub-signer
registration. The System Bridge verifies PackageManager identities; generated
stubs may request only their own installation/update. No runtime-directory,
Downloads, kill or launch-status RPC remains. Rebuild bridge, linuxd and host
with the same AIDL: the interface is private and pre-alpha.

Apps and runtimes use the shared encrypted `/data/linux-app` store. Current
app-id pointers select immutable random version directories; read-only leases
protect running versions and their runtime references. PackageManager owns
per-user stub HOME/cache/runtime data. See [storage](STORAGE-LAYOUT.md),
[ownership](APP-OWNERSHIP-r24.md) and [publication](CODE-STORAGE-r24.md).

`flatpak-env-wrapper` is management-only and rejects `run`. linuxd prepares
fixed, system-owned mode-0700 `/data/linux-app/manager/{home,cache,run,tmp}`
scratch directories. The pinned stub certificate is in
`/data/linux-app/store/stub-signer.der`. No `/data/matonos/linux` tree remains.
The staging helper enters fixed UID 2902 (`AID_VENDOR_MATONOS_FPINSTALL`):
SETUID/SETGID remain only for that identity change. CHOWN/FOWNER seal staged objects; SYS_ADMIN
and KILL capabilities and the uinput group are removed. Network policy remains
for remote-add's HTTPS fetch. No app runtime or hardware proxy runs in linuxd.

Kept code: FlatpakManager/Publish/Store (management), RuntimeRefs and
DeploymentStore (lease-aware collection), StubVerify (certificate + live PM
identity), SafePath and FlatpakIcon (confined metadata/icon reads), LinuxStorage
and linux-paths (fixed storage layout), management AIDL and init registration.
Removed v2 launch code, input discovery/pads, Downloads grants, GTK/MLS helpers
and their tests. No new SELinux exceptions were introduced.

Local validation and exact logs are listed in
`out/pc-logs/agents/v2-retire-result.md`; policy/image and enforcing runtime
validation belong to the coordinator. Host compilation is not proof of device
behavior. Fresh-image steps: build policy and all related artifacts together,
run the label checker, boot with fresh userdata, disable idle suspend, install
one X11 and one Wayland app through the store, and verify private runtime
sockets, portals, audio and DNS. Repeat update/lease/multi-user removal checks
from STORAGE-LAYOUT. Confirm another UID cannot open runtime sockets and no
retired path or helper appears. Repeat on Ryzen/RX6600, Surface Pro 3 and
HP ProDesk; no kernel change or hot-patching is required.

Round 20 install failure recovery:

`install/<appid>-<six alphanumeric characters>` is an immutable deployment
version, not installer staging. Failed creation/publication removes an
unpublished version immediately. Startup reconciliation also collects abandoned
versions, including interrupted creation before `.lease` exists, without needing
PackageManager to answer. Collection is fd-relative, never follows directory
symlinks or crosses filesystems, and preserves the current pointer target and
any version with a live shared lease. Only the exact mkdtemp name shape is swept;
unrelated entries are left alone.

`install/<appid>` is the atomic symlink committed after successful publication.
A fresh failed install must not create it; a failed replacement keeps the prior
successful deployment. Queries reject missing pointers and numeric directories
before invoking Flatpak, so Flatpak cannot initialize a bogus numeric install.
If the final commit rename has already succeeded but its directory fsync fails,
the selected version is preserved: it may already be in use. This is reported
as a commit failure, never handled by deleting a live target.

The debug harness reads root/shell-only JSON from `dumpsys
org.matonos.systembridge.ILinuxd/default`, keyed by operation ID. linuxd retains
up to 64 recent operation snapshots, reports staging progress and terminal
errors, and ignores progress redraws that change only speed/ETA. Staging uses
`--assumeyes` rather than the quiet `--noninteractive` transaction so download
progress is available. `MATON_FLATPAK_IDLE_MINUTES` (positive integer, default 10)
controls inactivity rather than total install duration. Progress is the CLI's
reported bar/percentage or a changed operation phase, so an extremely slow
transfer that cannot advance the reported bar within that interval needs a
larger setting. Snapshots are diagnostic, not a new privileged control API.

Host regressions: `deployment-store-test.py`, `failed-publication-test.py`,
`operation-wait-test.py` and `staging-progress-test.c` cover partial creation,
failed publication, current/leased preservation, startup recovery, query safety,
long advancing operations, stalled operations, fast errors and CLI output
framing. Exact run results and existing suite blockers are recorded in
`out/pc-logs/agents/v3-install-result.md`.

Fresh enforcing-image check for the coordinator (no hot-patching):
1. Install a nonexistent ref such as `app/org.codeberg.dnkl.foot/x86_64/stable`;
   verify a quick operation error and no new pointer or random deployment.
2. Interrupt publication, reboot, and verify only unselected/unleased versions
   disappear. Repeat while another app version is leased and while PM is not
   yet reachable. Verify a failed update preserves the working current version.
3. Install a large Flathub app with a slow link. Confirm advancing snapshots
   keep the harness waiting past ten minutes. Stall the link and confirm the
   configured inactivity limit; repeat with a small limit for a quick check.
4. Repeat failure/success/update on the Ryzen PC, Surface Pro 3 and HP ProDesk.
   Inspect enforcing denials for the dump/progress path; no kernel changes or
   shared-file changes are required by this source change.

### Round 12 phase 1: partial store groundwork (2026-10-14)

This historical phase did **not** complete the store API. Round 27 below
replaces its early-stub flow and completion refresh; no ILinuxAppStore service
has been added in this install-ordering change.

linuxd now checks a system-wide application-origin pin before staging and
again under its publication mutex. After verified deployment, it persists the
pin before switching the active deployment pointer. Metadata lives in
`/data/linux-app/manager/<appId>` (private to linuxd); the file contains the
remote name. Branch, architecture and Android user do not change the pin.
An existing deployment's Flatpak origin is checked when upgrading from an
image without these files. A conflicting remote fails closed. Pins survive
uninstall: allowing an intentional provenance change needs an explicit future
policy. A pin can also survive a failed final pointer switch; retry from the
same remote is allowed. New metadata uses write/fsync/link/fsync so a crash
cannot expose a partially written pin.

The bridge's existing management and root debug-provider paths now enforce
`DISALLOW_INSTALL_APPS` for install/update commands and
`DISALLOW_UNINSTALL_APPS` for removal. Cross-user management checks both the
calling and target users. The debug entry point still requires its existing
root/live/debug arming and is otherwise unchanged.

Host verification: remote-pin-test.c, system-wide-management-test.py,
deployment-store-test.py, failed-publication-test.py and operation-wait-test.py
pass. FlatpakManager.c passes NDK r30/API 36 syntax compilation with
`-Wall -Wextra -Werror`. The modified bridge service passes focused javac
against existing framework/bridge jars and SDK 36; LinuxAppStorePolicyTest
passes both JUnit tests. No image build or device test was performed.

Remaining Store API work: add the permission-protected AIDL API and update
management/admin-only all-users removal. The installer identity and final
stub handoff are implemented by round 27 below.

Device checks for the coordinator after a fresh image build (not run here):
1. On a disposable user, install from a signed configured remote using the
   existing `install/tools/test-flatpak.sh` harness; confirm a private pin file
   names that remote and the application launches.
2. Attempt the same app ID from a second signed configured remote, including
   another branch; expect an explicit remote-pin failure, unchanged deployment
   pointer and unchanged first remote's app.
3. Reboot and repeat the conflict, then remove and retry from the original
   remote. Verify the pin persists across reboot/removal.
4. Set `no_install_apps` for the target user; the armed root harness and normal
   bridge install must both reject before scheduling work. Clear it and retry.
   Repeat removal with `no_uninstall_apps`. Check cross-user calls cannot avoid
   the caller's restriction by targeting another user.
5. Repeat on the Ryzen desktop, Surface Pro 3 and HP ProDesk when images are
   available. These changes have no hardware-specific behavior.

No SELinux lines or kernel settings were changed. These statements describe round 26 only. Round 27 changes policy and the
install harness; see its verification limits below.


### Round 27: FPINSTALL staging, verified deployment, final APK once

New installs no longer need an application UID. The bridge schedules a
`deploy_pending` operation; linuxd stages only as UID 2902, under
`/data/linux-app/staging/2902/<operationId>`. Bionic Flatpak uses netd directly,
with the inet group. Flatpak's own transaction pulls dependencies; linuxd
publishes only the transaction's observed refs rather than resolving runtimes
itself. Stage metadata is diagnostic/untrusted and never feeds stub generation.

Under the publication mutex, linuxd imports with `pull-local --untrusted
--gpg-verify` using its own configured remote/keyring, verifies checksums,
seals objects, deploys into an unpublished `.pending-<FlatpakID>-XXXXXX`
installation, sanitizes checkouts and persists the system-wide remote pin.
Only then can the bridge request operation-scoped metadata, desktop text and
PNG icon. Missing icons use the existing generated fallback. Unknown operations,
refs and remote mismatches fail closed; arbitrary installation paths, signing
keys and installer UIDs are not handoff inputs.

Sanitization rejects setuid/setgid, devices/FIFOs, extra xattrs, filesystem
crossings and escaping relative links. Standard runtime `/usr`, `/etc`,
resolver `/run/host/monitor/resolv.conf` and application `/app` absolute links
are replaced in the checkout with confined relative links. Repository objects
are never rewritten. Relocated runtime `etc` retains its relative namespace
references; other absolute targets fail closed. Real launch/DNS compatibility
still needs the fresh-image check. The unpublished filename limits Flatpak IDs
to 220 characters; oversized requests fail without installing an APK.

The bridge generates the final label, permissions and icon from that verified
deployment, and commits one PackageInstaller session. The launcher stays disabled
until linuxd verifies the assigned UID against the live PM identity provider,
renames the version into the normal numeric deployment family and atomically
binds `install/<appId>` to it. Refcounts receive the real application ID only at
binding. The private handoff protects runtime GC in the meantime; verified
runtime refs are recorded as cleanup candidates before deployment.

An existing signed stub receives a new deployment pointer without an APK
replacement. An existing global package belonging to another Android user is
installed for the target user with `installExistingPackage`, avoiding a global
APK replacement. Completion no longer triggers label/permission refresh.
Consequently, an update does not change an existing stub's label, icon or declared
permissions; explicit future migration is needed for those changes. A changed
branch/ref in an existing stub is rejected at binding rather than trusted blindly.

Stage, verification, deployment, metadata/APK generation and PM rejection all
abort the private deployment. Failed binding removes a newly installed stub;
updates preserve the previous stub/pointer. A lost completion-report reply does
not remove a successfully bound pair. If pointer rename committed but directory
fsync fails, the selected pair is retained and the durability error is logged.
Startup collects unpublished named versions as well as abandoned numeric versions.
A replacement bridge identity aborts its previous unpublished PM handoff; bridge
reconciliation removes signed hidden stubs with no binding (there is no valid
placeholder anymore).
Cleanup I/O/PM failures are logged for reconciliation rather than blocking boot.
Daemon crash/restart between PM commit and UID binding, and real PM cancellation,
remain device checks; the host doubles are not proof of crash atomicity across PM.

Installer policy now has a dedicated staging file type. Its network domain can
use netd DNS and HTTPS; it can search the store root but cannot write store,
deployment or code files. linuxd can read/reclaim staging. The existing installer
process transition is retained; no binder/API rights or new policy files were
added for the downloader.

Host verification (exact evidence in `out/pc-logs/agents/v3-fpinstall-result.md`):
- `fpinstall-handoff-test.py`: real C worker with CLI doubles; staging/resolve,
  signature/deploy, unsafe absolute/relative links, FIFO, modes/xattrs, metadata,
  simulated PM rejection, binding, restart collection, success and failed update.
- `final-stub-once-test.py`: production bridge methods with PM/generator doubles;
  correct final metadata/permissions, all handoff failure points, one APK commit,
  no APK commit on update and no completion refresh.
- `fpinstall-signature-test.py`: real cached signed Calculator objects;
  malformed signature, changed content checksum and incorrect linuxd keyring
  rejected; replacing only the staging keyring cannot change trust.
- Existing deployment, query, refcount, remote-pin, operation-wait and progress
  regressions; NDK object/helper compilation and focused Java compilation.
- Fast m4/checkpolicy/sepolicy-analyze/secilc replay against the last platform
  policy, including neverallows and file_contexts. Coordinator policy/image
  validation is still required after merge.

Fresh enforcing-image steps (coordinator/user only; no hot-patching):
1. Build linuxd, the Flatpak APEX helper, bridge, policy and image together. Run
   the label checker. Boot fresh userdata; disable idle sleep. Use a disposable
   target user and root-arm the existing harness.
2. Run `install/tools/test-flatpak.sh --install app/org.gnome.Calculator/x86_64/stable`
   (set `MATON_ADB_SERIAL`, `MATON_ANDROID_USER` as needed). It polls the operation
   through staging/handoff/completion and no longer starts an INSTALL activity.
   During download assert no `flatpak.org.gnome.Calculator` package/UID or numeric
   pointer exists, and the pull has UID 2902 plus installer SELinux domain.
3. Inspect final label, PNG and declared permissions against deployed signed
   metadata. Launch immediately after visibility; check there is only one PM
   APK install and no `stopPackageTimeout`/completion replacement.
4. Repeat install/update while the app runs. Confirm its PID and APK stay the
   same, the numeric pointer changes, and its lease keeps old code/runtime refs.
   Test another Android user: the global APK must not be replaced.
5. Test nonexistent refs, unsigned/incorrect-key commits, damaged staging content,
   denied PM installation and interrupted publication/binding on disposable data.
   Verify failure snapshots and no new stub or unpublished app version; a failed
   update must preserve the prior working pair. Reboot/restart during the PM
   handoff and inspect both hidden stubs and abandoned versions explicitly.
6. Run `--uninstall` with the same ref. Confirm PM removes the user's stub, removes
   its app data, and collection respects other users and live leases. Repeat
   restriction and remote-pin conflict checks from round 26.
7. Launch both X11 and Wayland apps, verify runtime symlinks and sandbox DNS, then
   repeat on Ryzen/RX6600, Surface Pro 3 and HP ProDesk. No kernel changes or AOSP
   patches are needed. No adb/device execution was performed for this round.
