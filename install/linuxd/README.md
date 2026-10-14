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
The staging helper enters the verified stub UID for network attribution:
SETUID/SETGID remain for that job. CHOWN/FOWNER seal staged objects; SYS_ADMIN
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
