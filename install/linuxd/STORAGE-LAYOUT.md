# Linux app storage layout (2026-10-07)

Android user `<u> = uid / 100000` owns two encrypted roots, created by
`vold_prepare_subdirs` as 0711 system:system. linuxd never creates these roots.

```
/data/misc_de/<u>/matonos-linux/
  install/<uid>/           # per-app FLATPAK_USER_DIR, system-owned
    repo/objects/          # verified code label
    staging/<operation>/   # transient; handed to the installing stub UID
    app/ exports/ overrides/ tmp/
  runtime/<runtime_uid>/   # per-user shared FLATPAK_SYSTEM_DIR, system-owned
  runtime/refs-<u>.json    # reference DB, opened only during each operation
  runtime-installation    # system-owned runtime UID marker (0600)
  store/apps/ store/runtime/ # reserved code-store locations
/data/misc_ce/<u>/matonos-linux/
  home/<uid>/              # HOME, uid:uid 0700, per-UID MLS label
  home/<uid>.owner         # system-owned recycled-UID protection
/data/matonos/linux/
  tmp/<uid>/               # UID-owned app runtime sockets (media_rw_data_file)
  run/                     # CLI namespace configuration and relay state
  cache/ flatpak-data/     # linuxd CLI scratch/configuration
  machine-id udev/         # device-wide
  store/stub-signer.der    # device-wide signing certificate
```

Every installation and home parser checks the numeric user against the UID
and rejects noncanonical paths, suffixes and the previous global roots.
Install/runtime data uses DE keys; homes use CE keys. Launch fails with an
unlock message when the CE root cannot be opened. No migration is provided;
missing installations and reference databases are empty state.

vold destroys the entire DE and CE trees on user removal. linuxd holds no
refs cache; operations release their database on completion. Pending consent
handles are dropped when the numeric DE user directory is removed. Android
terminates that user's stubs; lifeline EOF terminates their Linux sessions.

Directory creation uses thread-local fscreate for install/runtime deployment
roots, staging and per-UID homes. DE parents and CE home parents are created
lazily inside the vold roots; neither is created by init. Existing state is
not moved as a migration. Stock vold recursively restores misc_ce labels at
user storage preparation (FsCrypt.cpp:1060); this resets dynamic MLS to s0.
On launch linuxd repairs only its own data type back to the verified UID
level, walking pinned inodes as that UID, without following symlinks or
crossing filesystems. It labels the root last so interruptions retry.
Unknown labels, foreign-owned descendants and unreadable directories fail
closed. This maintenance path requires enforcing/device validation.

## Standard graphics runtime

Every runtime declaring `[Extension org.freedesktop.Platform.GL]` also gets
`org.freedesktop.Platform.GL.default` (Mesa) in the shared runtime install.
The architecture comes from the runtime ref; the GL branch comes from its
authenticated metadata (`versions` first, legacy `version` otherwise).
For example, KDE `5.15-25.08` uses GL `25.08`, not `5.15-25.08`.

Prepare downloads it as the installing stub UID. Publish verifies the staged
extension with the remote keyring, seals its objects, and deploys it offline
before the runtime and app. Missing, unverified or undeployable Mesa makes
the install fail. Runtimes without a GL declaration do not acquire one.

Focused checks (outputs outside source):

```
cc -Wall -Wextra -Werror install/linuxd/tests/gl-runtime-test.c -o "$AOSP_ROOT/out/gl-runtime-test"
"$AOSP_ROOT/out/gl-runtime-test"
python3 install/linuxd/tests/gl-publish-test.py "$AOSP_ROOT/out/gl-publish-tests"
```

The publication test uses a fake CLI to check command ordering and error
propagation; real signature and graphics verification still require a built
image. The current VM's Mesa install and Firefox GPU mappings were checked.

## Why this shape (verified in the Flatpak source)

- `overrides/` is `<FLATPAK_USER_DIR>/overrides` (`flatpak-dir.c:2809`), **read-only at
  run**, written only by `flatpak override` — so it stays system-owned.
- At run, Flatpak writes only under **HOME** (`flatpak_get_data_dir` =
  `$HOME/.var/app/<id>`; `flatpak_ensure_data_dir` makes `data/`,`cache/`,`config/`,
  `cache/tmp`) and under `$XDG_RUNTIME_DIR/.flatpak/<id>/tmp` for shared tmp. The
  sandbox `/tmp` is bwrap `--dir /tmp` (ephemeral, private).
- `flatpak-run.c` never references the install root, so the install/runtime trees are
  read-only at run. `repo/tmp` is OSTree staging, used at install/update only.
- So **no directory inside `install/`/`runtime/` needs to be app-writable**; all
  app-writable state is `home/<uid>`.

## Known gaps / follow-ups

- **Enforcing validation pending.** Directory creation now uses fscreate;
  the new vold root labels and traversal/removal rules are text-reviewed only.
  Run the policy target and fresh-user device checks with the coordinator.
- **Per-app runtime dir (decided 2026-10-07).** The Wayland socket and session
  bus live in `/data/matonos/linux/tmp/<uid>/`. linuxd pre-creates the root
  as UID:UID 0700 with `media_rw_data_file` and the verified UID's stock
  `levelFrom=all` categories (2026-10-11). The stock untrusted app stub creates
  its children, with fixed names `wayland-0` and `bus`; the compositor changes
  the root to 0711 for the UID-1000 supervisor's control-socket traversal.
  This is a shared coarse internal-storage label: DAC confines contents, and
  socket MLS constrains connections. No custom runtime type or app MLS trust.
  Old/wrong root labels fail closed; no relabel migration is provided.
  See `docs/LINUX-STUB-SELINUX.md`. The
  sandbox identity-binds that dir and uses it as `XDG_RUNTIME_DIR`. This replaces
  the earlier shared `run/` relay namespace (the relay and `run/wayland-*` /
  `run/session-bus-*` are gone), because socket creation moved from `linuxd` into
  the app's in-process runtime. `run/` is now only for `flatpak-config-*`.
  See `docs/HANDOVER-2026-10-07-per-app-runtime-sockets.md`.
- **Prebuilt helpers.** `linux/flatpak/prebuilt/static/x86_64/{matonos-bwrap,
  flatpak-env-wrapper,matonos-flatpak-store}` embed the old paths; rebuild them from
  source (MatonOS_apexs `flatpak/build-helpers.sh`) and refresh
  `prebuilt/static/SOURCE`.
- **Cross-app isolation (security, user-requested 2026-10-07).** One app UID must not be
  able to read another app's install (`install/<other>`) or home (`home/<other>`). The
  per-UID MLS categories (`linux_data:<mls>`) are the intended mechanism, but this is
  not yet enforced/verified (permissive SELinux does not enforce it). Verify with AVCs
  and add `neverallow`/constraints so a compromised app in `matonos_linux_app` cannot
  read `install/[0-9]+` or `home/[0-9]+` of a different UID.
