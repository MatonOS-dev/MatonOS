# r24: Flatpak code storage and verified execution

> **Historical design on the static-apex branch.** Phase 1 removes the
> `matonos-mount-helper` launch protocol and its APEX binary; the static stack
> uses Flatpak's normal deployment paths. Keep this document as the record of
> the former r24 loop-image design. Do not treat the helper/domain flow below
> as the current launch chain. See [`linux/flatpak/APEX.md`](../../linux/flatpak/APEX.md).

This resolves the H6/app-owns enforcing blocker (`domain.te` forbids executing
data_file_type from /data and forbids writing exec_type) **without any platform
policy exception**. It implements the design decided 2026-10-04 (NOTES.md).

## Why the previous design was blocked

Two stock `system/sepolicy/private/domain.te` neverallows encircle /data code:

1. `neverallow { domain -appdomain ... } { ... }:file execute;` and the /data
   variant forbid a non-appdomain from executing a `data_file_type`.
2. `neverallow { domain ... } { system_file_type vendor_file_type exec_type }:
   dir_file_class_set { ... write ... }` forbids every domain from writing an
   `exec_type` (so writable+executable is impossible).
3. `full_treble_only`: a `coredomain` may only take `file:entrypoint` from
   `system_file_type` or `postinstall_file`; a non-coredomain only from
   `vendor_file_type`/`init_exec`.

The old code therefore deliberately left deployed-code execution denied.

## Resolution: mount labels, not relabels

Flatpak code is stored in sparse images on /data and **loop-mounted with a
mount-time label**, exactly like vold's AppFuse:

```
fscontext=u:object_r:matonos_code_fs:s0
context=u:object_r:matonos_app_code_exec:s0:<stub MLS categories>
```

* The superblock label (`fscontext=`) carries `contextmount_type`
  (`domain.te` restricts context mounts to that attribute). Stock policy only
  lets a non-allowlisted domain mount `sdcard_type`/`fusefs_type`
  filesystems, so the image also carries `fusefs_type`, just as FUSE mounts
  do; the installer still needs explicit `mount`/`relabelto` grants.
* The inode label (`context=`) is an **`exec_type`, not a `data_file_type`**,
  so the /data execution neverallow does not apply.
* The image is mounted **read-only**, and writing `exec_type` is neverallowed,
  so verified code cannot change under a running app.
* Because the label comes from the mount (kernel-applied), no policy
  `relabelto` on an exec_type inode is needed; the only checked permission is
  `filesystem relabelto` on `matonos_code_fs`, which the installer holds.

### Payload entry without file:entrypoint

`matonos_app_code_exec` is a genuine exec_type and is never mislabelled as
system/vendor code, so it cannot be a `coredomain` entrypoint. bwrap instead
execs `matonos-app-exec`, a tiny system_ext launcher that runs in the narrow
`matonos_app_launch` domain. That domain is `mlstrustedsubject` for MLS only
(the kernel's `mlsconstrain process { transition dyntransition }` requires
equal levels unless the source is trusted, exactly as with `zygote`), so it can
move to the stub's per-app level. The launcher **mounts nothing and holds no
capability**: it verifies its own `matonos_app_launch` label and the per-app
level derived from the UID it actually runs as (the verified stub UID), then
`setcon()`s to the untrusted app domain at that level and `execve()`s the
payload with `execute_no_trans`. No file entrypoint is involved and the app
domain keeps no setup rights.

The verified code image and optional volume are mounted **from outside the
sandbox** by `matonos-mount-helper` (domain `matonos_mount_helper`), the only
holder of `CAP_SYS_ADMIN` on the launch path. bubblewrap is started with
`--info-fd` (sandbox child PID) and `--block-fd` (it waits before exec'ing the
payload); the privileged side `setns()`es into `/proc/<child>/ns/mnt`, mounts
the installer-attached images with `context=` labels, and only then releases
`--block-fd`. No capability, loop device or `mount(2)` ability ever enters the
sandbox. See `out/pc-logs/agents/ds-setns-result.md`.

## Domains

| Domain | Entered by | Purpose |
| --- | --- | --- |
| `matonos_linuxd` | init | trusted coordinator; owns the Bridge transport and execs the installer helper |
| `matonos_flatpak_installer` | exec of `matonos-flatpak-store` | **only** writer/attacher of the store images; runs Flatpak management |
| `matonos_flatpak_run` | dyntransition (verified stub) | Flatpak CLI while it prepares one sandbox |
| `matonos_bwrap` | exec of bwrap/matonos-bwrap | user namespaces and mounts only; no app data, no binder |
| `matonos_mount_helper` | exec of `matonos-mount-helper` | privileged outside-sandbox mounter: setns into the sandbox mount ns, mount verified code/volume; no app data, no binder |
| `matonos_app_launch` | exec of `matonos-app-exec` | trusted-for-MLS-only launcher: verify own label/level, dyntransition to the app domain, exec the payload; no mount, no capability |
| `matonos_linux_app` | dyntransition in `matonos-app-exec` | payload: verified code, its volume, its sockets, /dev/dri, execmem; zero binder |

Every sandbox domain (`matonos_linux_app`, `matonos_flatpak_run`,
`matonos_bwrap`, `matonos_app_launch`) is covered by neverallows that forbid
all Android Binder, binder-device and service-manager access beyond the
unavoidable stock baseline. The stock platform policy unconditionally grants
every domain `system_server:binder call` and `rw_file_perms` on
`binder_device`/`hwbinder_device` chr_file (`private/domain.te`); those grants
cannot be subtracted from private policy, so the assertions carve out exactly
that baseline, and the runtime seccomp filter rejects every Binder ioctl, which
is the real enforcement. An app can never re-enter the bwrap setup domain or
execute bwrap itself (nested sandboxes go through `flatpak-portal` under
linuxd).

## Stores

```
/data/matonos/linux/store/runtime.img        shared runtime/extensions (ext4)
/data/matonos/linux/store/runtime/           its mount point (host namespace)
/data/matonos/linux/store/apps/<appid>/code.img   per-app verified code (erofs)
/data/matonos/linux/store/apps/<appid>/vol.img    optional writable volume (ext4)
```

* `matonos-flatpak-store` builds an erofs image from the Flatpak deployment
  (`mkfs.erofs`), enables fs-verity on the image file (`FS_IOC_ENABLE_VERITY`;
  loop reads then go through the kernel verifier), writes a SHA-256 sidecar,
  and renames the pair into place atomically. Growing is sparse
  (`ftruncate`); removal trims with `FITRIM`.
* The installer only **attaches** a per-app image to a free loop device
  (`app-attach`/`vol-attach` print the loop path and record it in the app's
  store directory; `loop-detach` clears both). It never mounts per-app code in
  the host namespace. The **privileged `matonos-mount-helper`**, outside the
  sandbox, `setns()`es into the app's sandbox mount namespace and attaches the
  loop image there with
  `context=u:object_r:matonos_app_code_exec:<stub level>` (volume:
  `matonos_app_volume_file`), so the mount exists only in that sandbox and is
  never reachable at a shared path. The helper receives only the verified app
  id and a pidfd over an unnamed socketpair created by the launcher; it
  verifies the sandbox process itself (`matonos_bwrap`, expected stub uid,
  descendant of this launch's wrapper, foreign mount namespace), derives the
  loop device from the attach record and uses fixed targets (`/app`, the app's
  Flatpak data directory). The MLS level is computed from the verified stub UID
  (`MatonMls.h`, mirroring AOSP `android_seapp.c` `levelFrom=all`), never from
  a string supplied by the sandbox. `matonos-app-exec` mounts nothing and needs
  no capability. The shared runtime is the one exception: it is mounted once
  in the host namespace as `matonos_runtime_exec:s0` and is readable and
  executable by every app (every app level dominates `s0`).
* The tool pins: newest stable `erofs-utils` for `mkfs.erofs` (shipped as a
  system_ext prebuilt) and the kernel's fs-verity; where /data lacks fs-verity
  the helper records an explicit degraded mode and still checks the sidecar
  digest at mount. dm-verity is the fallback if fs-verity is unavailable.
* The writable+executable volume of the original design is **not**
  simultaneously writable and executable: that is impossible under stock
  neverallows. `vol.img` is a persistent writable **data** volume
  (`context=matonos_app_volume_file`); code an app downloads into it is made
  executable only after the installer **seals** it into a new verified
  `code.img` (atomic swap). This mirrors ChromeOS and keeps the neverallows
  intact.

## Planned: app installations per stub uid + one runtimes installation, hardlinked checkouts (decided 2026-10-04)

Supersedes "images carried in APKs" (retired: erofs images in APK entries,
zip reader and loop setup in the mount helper) and per-version
static-library runtime stubs (retired). Kept: app stub generation and the
bridge's install flow. Goal: as much unmodified Flatpak plumbing as possible.

* **Layout.**
  ```
  /data/matonos/linux/apps/<app uid>/            Flatpak USER installation of one app
    app/<id>/<arch>/<branch>/<commit>/           files/ metadata export/ deploy
    app/<id>/<arch>/<branch>/active -> <commit>
    home/                                        the app's writable data
  /data/matonos/linux/apps/<runtimes app uid>/   Flatpak SYSTEM installation: every runtime
    runtime/<id>/<arch>/<branch>/<commit>/ (+ active)
  ```
  "MatonOS Linux Runtimes" is ONE preinstalled app (in the image, not
  uninstallable, fixed identity) that owns all runtimes; it shows as one entry
  in Android's storage settings. At launch linuxd sets
  `FLATPAK_USER_DIR=apps/<app uid>` and `FLATPAK_SYSTEM_DIR=apps/<runtimes uid>`
  (bound read-only), so `flatpak run` resolves app and runtime natively. Every
  sandbox can see all runtimes (public content, read-only). Updates: new
  `<commit>/` beside the old one, flip `active`; running apps keep theirs.
* **Proven on the host with stock Flatpak 1.16.6 (layout spike, 2026-10-04,
  out/pc-logs/agents/layout-spike-result.md):** `ostree pull-local` from the
  shared repo into each installation's own `repo/` hardlinks every object (0
  bytes written); stock `flatpak install --no-pull` then deploys (deploy file,
  exports, `active`); `flatpak run` works with both installations bound
  read-only and writes nothing to them; GNOME Platform 51 + Calculator = 1.7 GB
  instead of 3.5 GB. `install --no-pull` needs a system bus only for malcontent:
  build Flatpak with `-Dmalcontent=disabled`. The mount helper's code mounting
  is no longer needed (flatpak run binds /app and /usr itself).
* **Code is a hardlinked OSTree checkout** (`ostree checkout -H`): no doubled
  disk, dedup across apps/runtimes, repo always kept (delta updates).
* **App stubs are the signed record.** Each app stub carries its app commit
  checksum and its runtime ref + commit in the manifest, signed with the
  per-device stub key. Runtimes are verified against what the installing
  app's signed stub declares; the runtimes app itself carries no record.
* **Install = verify, freeze, publish.** The installer pulls into staging R
  with a staging label it can write. linuxd checks the staged ref against the
  commit named by the signed stub, then imports it into the target installation
  with `ostree pull-local --untrusted --gpg-verify --remote=<r>`. GPG verifies
  the signed commit; `--untrusted` makes OSTree verify each source object's
  checksum as it is imported. Because those checks cover the full object
  closure being transferred, a separate `ostree fsck` would only rescan the
  target repository (and the static CLI's `fsck` has no commit selector).
  linuxd then relabels the imported objects to `matonos_linux_code_file`
  (installer cannot write it; OSTree never rewrites objects), hardlink-checks
  out into the right installation, and flips `active`. Verification and
  checkout use the same frozen bytes.
* **Labels.** `app/`, `runtime/` trees: `matonos_linux_code_file` at `s0`,
  read/execute for `matonos_linux_app`, written only by linuxd's publish step.
  `home/`: `matonos_linux_data_file` at the app's MLS level.
* **Lifecycle.** App stub removal -> bridge reconcile -> linuxd deletes
  `apps/<uid>/`. Runtime GC (ours): delete runtime commits that no installed
  app stub declares and no running sandbox binds; deleting only removes
  links, open files survive. Boot sweep; project-quota tagging per uid.
* **Safety rules.** Never modify a published `<commit>/`; linuxd never follows
  symlinks (reads `active` itself, validates a 64-hex commit); reject extra
  files, setuid, device nodes, hardlinks outside the repo. Later option:
  fs-verity on objects.

## Per-app Linux data and downloaded code (decided 2026-10-04)

**All per-app Linux state lives in `/data/matonos/linux/apps/<stub uid>/`** (user decision;
the stub's own app data dir was rejected: our domains may not touch
`app_data_file` dirs (`domain.te` ~1786) and untrusted apps may not create
any other label there (`app_neverallows.te` ~194); symlinks don't help).
- The existing `/data/matonos/linux/apps/` tree, labelled via our `file_contexts` as
  `matonos_linux_data_file`; per-uid dirs are created by linuxd, owned by the
  stub uid, at the stub's MLS level (set the create context explicitly).
- Session sockets stay owned by the compositor host exactly as today (its
  runtime/X11 dirs passed to launchOwnedFlatpak; user decision 2026-10-04
  after the host audit) — no per-uid run/.
- `home/`: the Flatpak app's data/home (replacing the per-app dirs under
  /data/matonos/linux and `vol.img`), including any code it downloads.
- Lifecycle: the bridge's stub reconcile (FlatpakStubManager) asks linuxd to
  delete `/data/matonos/linux/apps/<uid>` when the stub is removed (also on user removal);
  linuxd sweeps orphans at boot. Per-user automatically (uid encodes the user).
- Storage accounting: tag the tree with the app's filesystem project ID
  (`FS_IOC_FSSETXATTR`, inherit flag) so Android counts it as the app's data —
  verify the Android 17 project-ID scheme first.
- Not credential-encrypted per app (/data itself is encrypted).

**Flatpaks may download and run code in their own `/data/matonos/linux/apps/<uid>` by default**
(user decision 2026-10-04: Linux apps have no concept of a permission for
this, and on a Linux desktop an app can always run files from its home).
There is ONE sandbox domain, `matonos_linux_app` (renamed from matonos_flatpak_app): a non-appdomain with no
binder that may execute (execute, execute_no_trans, map) its own
`matonos_linux_data_file` tree (`/data/matonos/linux/apps/<uid>`, at the stub's MLS level). No
WRITABLE_CODE permission, no second domain, no prompt. Never `execmod`; never
write to any exec type. Containment stays: own uid, own MLS level, no binder,
only its own directory. Flatpak-installed code arrives from the app's Flatpak
deployment. Downloaded code in the app's own home is always allowed; there is
no runtime permission or separate writable volume. The
`data_exec_exempt_domain` patch covers code execution from app data.

This needs `patches/system/sepolicy/0001-data-exec-exempt-domain.patch` (user
decision 2026-10-04, second patch-budget entry): a private attribute
`data_exec_exempt_domain`, given only to `matonos_linux_app`, exempted from
the neverallows at `private/domain.te` ~1960 (non-appdomains execute only
exec/system/vendor files) and ~2040 (no execute of data_file_type). The
appdomain route was rejected: appdomains may only be entered from zygote
(`domain.te` ~1484) and may hold no capabilities (`app.te` ~600).

## Sandbox user

Flatpak generates `/etc/passwd` and `/etc/group`; the wrapper joins as the
stub UID (>= 10000) with supplementary groups reduced to inet plus (only when
granted) the controller GID, so the sandbox sees one ordinary unprivileged
user with no wheel/sudo/admin/adm membership and no sudo/su/pkexec path.

## Remaining integration notes

* The current APEX launch chain contains no mount helper. Stock `flatpak run`
  binds its read-only app and runtime deployments into the sandbox.
* Device matrix in `APP-OWNERSHIP-r24.md` still applies (this change adds the
  mount/verity and domain-entry steps; the identity/cgroup work is unchanged).

### linux-data implementation gate (2026-10-04)

The sandbox domain rename to `matonos_linux_app` is compiled and the actual
worktree policy passes the isolated policy rig. The per-app data layout remains
planned. Stock `private/app_neverallows.te:194` prohibits stock untrusted app
domains creating/unlinking a custom `matonos_linux_data_file` type. The exact
proposed custom-label policy produced 40 neverallow failures; the existing
0001 execution exception does not cover this rule. No additional AOSP exception
was made. A data-only seapp selector can leave the stock process domain intact,
but cannot resolve this file-type restriction. Implementation therefore needs
explicit approval to extend the existing patch, or a different labeling/domain
decision. The saved layout draft is incomplete and must not be applied as-is.

### Narrowed linux-data implementation (2026-10-04)

The compositor retains session socket ownership and launch directory/display arguments.
No per-UID run directory is created. Writable state is apps/<uid>/home; vol.img
is no longer attached. Linuxd creates the UID tree with the verified stub MLS level.
