# r24: Flatpak code storage and verified execution

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
| `matonos_flatpak_app` | dyntransition in `matonos-app-exec` | payload: verified code, its volume, its sockets, /dev/dri, execmem; zero binder |

Every sandbox domain (`matonos_flatpak_app`, `matonos_flatpak_run`,
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

## Planned: images carried in APKs (decided 2026-10-04)

Flatpaks are installed the way Android installs APKs: the verified code
images live INSIDE Android packages, and PackageManager does staging,
signature verification, atomic commit, updates, uninstall, dependency
tracking and storage accounting. We write no store, declaration, digest or
GC code of our own. This replaces the shared `runtime.img`, the per-app
`code.img` store files and the attach records above (they stay until the
new path is wired).

* **Install flow.** The glibc Flatpak installer (APEX
  `com.matonos.flatpak.glibc`, called only by MatonWaylandHost) pulls with
  `flatpak install --no-deploy`. Each commit becomes an erofs image via
  `ostree export <commit> | mkfs.erofs --tar` (no deployment, no hardlinks).
  The system bridge wraps the image into an APK signed with the per-device stub
  key (Android Keystore, as stubs are today) and installs it through a
  `PackageInstaller` session. The Wayland app does exported `.desktop`/icon
  work from the image; triggers are not run.
* **Packages.**
  - App: the stub APK (`hasCode=false`) carries `matonos/code.erofs`.
  - Runtime/extension: a static shared library APK
    (`<static-library name="<ref>" version="<n>">`) carrying
    `matonos/runtime.erofs`. App stubs declare `<uses-static-library>` (same
    certificate), so PackageManager installs runtimes first, refuses to
    uninstall one still in use, and may prune unused ones under storage
    pressure.
  - `apply_extra`: an `extra` split APK of the app's package carrying
    `matonos/extra.erofs` (see below).
* **Image entries.** Stored (no compression), 4096-byte aligned
  (`zipalign -p`, `setAlignmentPreserved(true)`), so they can be loop-mounted
  in place. The APK signature (v2/v3) covers the whole file, including the
  image. fs-verity on the APK is a later hardening step.
* **Mounting.** linuxd gets each package's `codePath` from PackageManager via
  the bridge (the trusted record; nothing from the app). `matonos-mount-helper`
  opens the APK with no symlink resolution under `/data/app`, checks owner
  and label (`apk_data_file`), finds the entry in the zip central directory,
  rejects it unless stored and aligned, then `LOOP_CONFIGURE`s a read-only,
  autoclear loop device from that same fd with offset + size limit and mounts
  it: code -> `/app` (`matonos_app_code_exec:<app level>`), extra ->
  `/app/extra` (same), runtime -> `/usr` (`matonos_runtime_exec:s0`).
* **Updates/uninstall.** A new APK version = new code path; running sandboxes
  keep the old file mounted until they exit (kernel keeps it alive). Uninstall
  removes everything; app data (`vol.img`, Flatpak data dir) is removed with
  the stub.
* **`apply_extra`.** After the code APK is installed: download extra data,
  check sizes/SHA-256 from the commit metadata, run the app's `apply_extra`
  through the normal launch chain as the app (no network, downloads bound
  in) writing into a scratch writable image mounted at `/app/extra` as data
  (never executable while writable), unmount, `mkfs.erofs` the result and
  install it as the `extra` split. App updates rerun it.
* **Costs.** Wrapping copies the image into the APK and signing hashes the
  whole file (seconds per GB, double disk transiently). Multi-GB images need
  zip64; to be checked on the VM.
* **No app-to-app code sharing.** Sharing stops at runtimes/extensions.

## Planned: per-app `linux/` directory and the WRITABLE_CODE domain (decided 2026-10-04)

**All per-app Linux state lives in the stub's own Android data directory:**
`<ApplicationInfo.dataDir>/linux/` (i.e. /data/user/<userId>/<stub package>/linux;
the path always comes from PackageManager via the bridge, never computed).
- `run/`: the session's XDG_RUNTIME_DIR content (`wayland-0`, `X11/X0`),
  created by the stub process (StubService runs as the stub uid); the
  listening fds go to the compositor host over binder; X display is always :0.
- `home/`: the Flatpak app's data/home (replacing the per-app dirs under
  /data/matonos/linux and `vol.img`); for WRITABLE_CODE apps also the code
  they download (e.g. a Steam library under their home).
Plain `app_data_file` at the stub's MLS level: only that app can touch it, and
Android handles multi-user, clear-data, uninstall and storage accounting.
Stubs set `allowBackup=false`. Not `code_cache/` (cleared on app update).

**W^X stays for every Flatpak by default** (`matonos_flatpak_app`; stock
`domain.te` 1960/2040/792 forbid non-appdomains executing any data or writing
any exec type). Apps granted `org.matonos.permission.WRITABLE_CODE`
(dangerous, user consent; generic rule from the Flatpak manifest, never per
app) run in `matonos_wx_app`: a NON-appdomain (no binder, keeps the
user-namespace capabilities nested sandboxes such as pressure-vessel need)
that may execute its own `app_data_file`. Never `execmod`.

This needs `patches/system/sepolicy/0001-data-exec-exempt-domain.patch` (user decision
2026-10-04, second patch-budget entry): a private attribute `data_exec_exempt_domain`, given only to `matonos_wx_app`, added to the
exception lists of the neverallows at `private/domain.te` ~1960 (execute only
exec/system/vendor files) and ~2040 (no execute of data_file_type). The
appdomain route was rejected: appdomains may only be entered from zygote
(`domain.te` ~1484) and may hold no capabilities (`app.te` ~600)
(DeepSeek wx-appdomain, verified). Flatpaks never get binder; no exception.

## Runtime permission

`org.matonos.permission.RUN_DOWNLOADED_CODE` (`dangerous`, declared by
`org.matonos.systembridge`) is requested by a generated stub only when the
Flatpak manifest declares a broad writable or persistent filesystem pattern
(`filesystems=host|home|~|~/...`, or any `persistent=`); default deny. Narrow
`ro` grants do not request it. See `StubGenerator.permissionsForMetadata`.

## Sandbox user

Flatpak generates `/etc/passwd` and `/etc/group`; the wrapper joins as the
stub UID (>= 10000) with supplementary groups reduced to inet plus (only when
granted) the controller GID, so the sandbox sees one ordinary unprivileged
user with no wheel/sudo/admin/adm membership and no sudo/su/pkexec path.

## Still to wire (r24 image / r25)

* Thread the RUN_DOWNLOADED_CODE grant through Bridge → linuxd → installer so
  `vol-ensure`/`vol-attach` run on first launch of an entitled app.
* Replace the single writable `runtime.img` with per-commit runtime images
  carried in APKs (see "Planned: images carried in APKs" below); today
  the installer seals the existing `/data/matonos/linux/flatpak` deployment.
* Wire the loop hand-off end to end: linuxd calls `app-attach`/`vol-attach`
  (which write the per-app attach records the helper reads), then launches the
  app; the launcher exports the unnamed socketpair fd that connects the
  `matonos-bwrap` shim to `matonos-mount-helper`, and calls `loop-detach` when
  the sandbox exits. No loop path or target is ever passed to the helper.
* The privileged mounter is `matonos-mount-helper`, exec'd by the launcher
  wrapper while it still holds `CAP_SYS_ADMIN`. It needs no capability grant to
  `matonos-app-exec` or any sandbox domain: it performs `setns(CLONE_NEWNS)`,
  `fsopen`/`fsconfig`/`fsmount` and `move_mount` itself. `matonos-flatpak-store`
  still holds `SYS_ADMIN` for loop attach and fs-verity. A file-caps/root
  service variant is the alternative if linuxd should not hold the capability.
* Device matrix in `APP-OWNERSHIP-r24.md` still applies (this change adds the
  mount/verity and domain-entry steps; the identity/cgroup work is unchanged).
