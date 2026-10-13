# System-wide Linux app storage (round 10, 2026-10-13)

Init creates `/data/linux-app` as system:system 0755 with
`encryption=Require` in post-fs-data. It is a sibling of PackageManager's
app-code directory, using the system DE policy. Failure never blocks boot;
linuxd operations fail if the prepared root is unavailable.

```
/data/linux-app/
  install/<appid> -> <appid>-<random>  # atomic current pointer
  install/<appid>-<random>/          # immutable Flatpak app installation
    .lease                          # stub's shared POSIX process-lifetime lock
    refs-0.json                     # this version's shared runtime references
    repo/objects/                   # verified code type
    app/ exports/ overrides/ ...
  runtime/                          # shared Flatpak runtime/extension installation
    refs-0.json                     # global app-id reference-count database
    repo/objects/                   # verified code type
  staging/<uid>/<operation>/         # temporary signed pull, as installing stub UID
  store/                            # reserved installer storage
```

`appid = uid % 100000`; an app installed for multiple users shares one code
installation. Temporary downloads retain the full UID for network attribution
and ownership. Empty staging-owner directories are removed after publication.
There is no permanent linuxd-managed user data. HOME is `<stub dataDir>/home`,
with cache in the stub's ordinary cache directory. PackageManager owns data
creation, encryption, labels, quotas and deletion. No migration is provided.

## Publish and lifetime

The store creates the device-signed stub first. Its verified UID/ref drives the
existing stage → install → stub-completion flow. linuxd imports pinned signed
commits offline, seals objects and deploys into a fresh random app-id directory;
it atomically renames a relative current-pointer symlink only after success.
Failure leaves the previous pointer intact. Flatpak's internal `--user` selector
is retained for the separate app root while `--system` selects shared runtimes;
both installations are system-wide and system-owned, with no app write access.
This preserves stock Flatpak's two-root lookup without a store-API redesign.

V3Launcher resolves current to a physical version, acquires a read-only shared
FileLock on `.lease`, rechecks current and keeps the channel for the entire stub
process lifetime. linuxd collects retired versions only with an exclusive POSIX
lock. Closing the last window, force-stop or crash releases the lease on process
exit. A pointer swap cannot redirect a running stub to a new deployment.

RuntimeRefs retains a manifest in each version plus a global reference database.
Publication protects runtime and extension refs before deployment. Reconciliation
merges all remaining version manifests before pruning; an old leased version
keeps its runtime refs. Missing databases are empty; corrupt inventories fail
closed. There are no new size/count caps. Stock Flatpak still owns runtime
commit deployment lifetime within its shared installation.

The bridge watches package removal for all users and periodically reconciles.
linuxd asks the authenticated bridge's live PackageManager provider whether any
user still occupies the app ID, conservatively preserving reused/shared IDs and
unknown state. It removes current only after all users remove the stub and its
lease is free. Inventories remain filtered to stubs installed for the requested
user; they do not create stubs in other users automatically.

## Policy

Existing deployment and verified-code types mirror apk_data_file ownership:
linuxd writes; untrusted_app_all gets directory/file read, map, execute and lease
lock, with explicit neverallows for writes and execute_no_trans. Staging/store
retain the installer data type. Generic linux-app contexts precede more specific
staging and repo-object contexts, avoiding equal-stem last-match overrides.
No home MLS repair, per-user roots, home type, vold storage grants or vold patch
remain. Bridge user enumeration uses its declared MANAGE_USERS permission.

Daemon scratch/config, device metadata and temporary per-UID sockets remain
under `/data/linux-app/manager`; those are not app runtimes, homes or installs.

## Verification and fresh-image checks

Host checks: full compositor/bridge Java, NDK API36 linuxd C/C++ objects,
helper builds, native build script, compat-proxy suite, signed-publication fake
CLI tests, RuntimeRefs tests, canonical paths, production cross-user queries,
and atomic-pointer/collector/lease/runtime-reference tests. Host tests do not
prove encryption, enforcing policy, real GPG pulls or graphics.

Coordinator must reverse any already-applied retired vold patch in its AOSP
checkout, rebuild all participants and the APEX helpers, and test a fresh image:

1. Check normal boot with no Flatpak installs, encryption policy and root labels.
2. Install and launch a small app; verify physical version paths, stub HOME/cache,
   runtime sharing, DNS, Wayland/X11, portals and an actual rendered window.
3. Install the same stub for a secondary user; verify one app-id deployment and
   distinct PM data. User-scoped inventories must contain only that user's stubs.
4. Update while the old stub runs: pointer changes, old version/runtime refs stay;
   close/relaunch, wait for reconciliation, and verify old-version collection.
5. Remove one user's stub, then remove a user: other users retain code. Remove
   the final stub: code and unused runtime refs disappear after leases release;
   PackageManager removes private data. Repeat after crash and force-stop.
6. Collect AVCs; another app may read shared code but cannot write it. Run the
   normal policy target and built-image label checker. Never grant deployment
   writes or execute_no_trans to fix a launch failure.

Run the same flow on Ryzen 5800X/RX 6600/Intel 7265, Surface Pro 3 and HP ProDesk
600 G1 after QEMU integration. No kernel changes or image/adb tests in this round.
