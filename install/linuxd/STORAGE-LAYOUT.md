# Linux app storage layout (2026-10-07)

Three roots, split by **owner** and **who reads them**:

```
/data/matonos/linux/
  install/<app-uid>/                      # per-app Flatpak --user install   (FLATPAK_USER_DIR)
    repo/objects/...                      #   -> matonos_linux_code_file
    staging/<operation>/                  #   transient, uid:system 0750   -> matonos_linux_data_file
    app/<app-id>/<arch>/<branch>/<commit>/files   # the deployment       -> matonos_flatpak_deployment_file
    exports/  overrides/  tmp/
  runtime/<runtime-app-uid>/              # shared --system runtime install   (FLATPAK_SYSTEM_DIR)
    runtime/<rt-id>/<arch>/<branch>/<commit>/files
  home/<app-uid>/                         # app data / HOME  (uid, 0700)      -> matonos_linux_data_file:<mls>
    .var/app/<app-id>/...
  # not part of any install:
  run/                                # shared, system-created socket namespace:
                                      #   wayland-*, session-bus-*, flatpak-config-*
  install/runtime-installation-<user> # runtime-owner markers (persistent, system)
  staging/  cache/  flatpak-data/  store/  udev/
```

| Root | Owner | SELinux type | App rights |
| --- | --- | --- | --- |
| `install/<uid>/**` | system | deployment (except `repo/objects` -> code, `staging` -> linux-data) | read + exec/map, no write |
| `runtime/<guid>/**` | system | deployment | read + exec/map, no write |
| `home/<uid>/**` | `<uid>`, 0700 | `matonos_linux_data_file:<mls>` | read/write/exec |
| `staging/**` | system | store | none |

`FLATPAK_USER_DIR=install/<uid>`, `FLATPAK_SYSTEM_DIR=runtime/<runtime-app-uid>`,
`HOME=home/<uid>`.

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

- **SELinux labelling at creation.** `file_contexts` applies only at build/`restorecon`;
  runtime-created dirs inherit the parent type. `home/<uid>` is labelled via `fscreate`
  in `prepare_linux_data`. The **`install/<uid>` and `runtime/<guid>` trees are created
  by the publish path and still need `fscreate`** (or a `restorecon` pass) to get the
  deployment/`code` types — otherwise the app cannot execute its own binaries. Do not
  assume `file_contexts` covers them.
- **Per-app runtime dir (decided 2026-10-07).** The Wayland socket and session
  bus live in `/data/matonos/linux/tmp/<uid>/`, created by the app (stub) itself
  as it runs under the app UID, with fixed names `wayland-0` and `bus`. The
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
