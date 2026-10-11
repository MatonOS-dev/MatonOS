# Per-Android-user Linux storage (plan, 2026-10-10)

Status 2026-10-11: source implementation and host validation completed;
vold patch prepared, not applied to AOSP. No builds or device tests.
See STORAGE-LAYOUT.md for the implemented roots and the agent result report
for full verification limits.

Goal: install files and app data belong to the Android user that owns the app,
instead of one global `/data/matonos/linux`. Decided: install files (apps,
runtimes, refs db) in DE storage, home/app data in CE storage. No migration
code; old VM data is wiped.

## Proposed roots

| What | Today | New |
| --- | --- | --- |
| per-app install (`FLATPAK_USER_DIR`) | `linux/install/<uid>` | `/data/misc_de/<u>/matonos-linux/install/<uid>` |
| staging | `linux/install/<uid>/staging/<op>` | same, under the new install root |
| shared runtime (`FLATPAK_SYSTEM_DIR`) | `linux/runtime/<runtime_uid>` | `/data/misc_de/<u>/matonos-linux/runtime/<runtime_uid>` |
| runtime-owner marker | `linux/install/runtime-installation-<u>` | `/data/misc_de/<u>/matonos-linux/runtime-installation` |
| code-store images | `linux/store/{apps,runtime}` | `/data/misc_de/<u>/matonos-linux/store/{apps,runtime}` |
| app HOME | `linux/home/<uid>` | `/data/misc_ce/<u>/matonos-linux/home/<uid>` |

`<u>` = `uid / 100000` (Android user id). The app uid already carries the user,
so every path is derivable from the uid alone, and the code must check that
the `<u>` in a path matches `uid / 100000`.

**Why `misc_de`/`misc_ce` and not a literal `/data/<user_id>/linux`:** vold
creates `/data/misc_de/<u>` and `/data/misc_ce/<u>` with that user's DE/CE
keys, and deletes them when the user is removed. That gives us encryption
with the right key, and cleanup on user removal, for free. A self-made
`/data/<u>` gets neither. **Decided 2026-10-10:** `matonos-linux` itself is
created by a `vold_prepare_subdirs` patch (DE and CE branches, 0711
system:system, labelled from `file_contexts`), so linuxd never needs write
access to `system_data_file`.

### Stays global (not per user)

- `linux/tmp/<uid>`: RAM tmpfs, per-uid already, the stub path in
  `StubService.java` is unchanged.
- `linux/run`: system relay sockets, `flatpak-config-<pid>`.
- `linux/cache`, `linux/flatpak-data`: the Flatpak CLI's own TMPDIR/HOME. It
  runs as linuxd, not as an app.
- `linux/machine-id`, `linux/udev`, `linux/store/stub-signer.der`:
  device-wide.
- `/dev/shm`.

## Consequences

- **Runtimes are per user (decided 2026-10-10).** Two users with Firefox
  each get their own runtime; fscrypt forbids hard links across user keys,
  so no dedup. Accepted: a MatonOS machine normally has one active user, and
  a real multi-user setup would be a managed/corporate one where a shared
  store does not fit either. No cross-user GC needed.
- **Confirmed by the user (2026-10-10): install files decrypt at device boot
  (DE), app data on user login (CE).** User 0's DE is available at boot.
  A secondary user's DE becomes available when that user is started; the
  user counts that (first login) as part of boot, so this is fine.
- **CE is only available after unlock.** HOME is created lazily on the first
  launch, which always happens after unlock. Install (DE) works before unlock.
- **User removal:** vold wipes both roots. linuxd only needs to drop its
  in-memory state for that user (refs db cache, running ops).

## Code map (literal `/data/matonos/linux` users)

| File | Uses | Change |
| --- | --- | --- |
| `install/linuxd/FlatpakManager.c` | 42 | install/runtime/home/marker builders, three `opendir(install)` scans, `per_app` prefix checks, `sscanf` owner parse |
| `linux/flatpak/flatpak-env-wrapper.c` | 25 | `sscanf` install/runtime validation (l.57), staging prefix (l.265); tmp/run/cache stay |
| `install/linuxd/FlatpakPublish.c` | 7 | runtime repo source, staging paths, `open(install)` dirfds |
| `linux/flatpak/matonos-bwrap.c` | 6 | HOME check `sscanf(.../home/%u)` (l.449); machine-id/udev/run stay |
| `linux/flatpak/app-session.h` | 2 | `data_dir`, `home` |
| `install/linuxd/MatonosLinuxd.cpp` | 1 | `opendir(install)` scan |
| `install/linuxd/FlatpakStore.c` | 1 | store images |
| `install/linuxd/MatonosLinuxd.rc` | 18 | drop `install`/`home`/`runtime`/`store/{apps,runtime}` mkdirs and restorecons |
| `systembridge/sepolicy/.../file_contexts` | 11 | new regexes under `/data/misc_(de\|ce)/[0-9]+/matonos-linux` |
| `systembridge/.../matonos_system_bridge.te` | 1 | path rule review |
| `ILinuxd.aidl` (both copies) | 1 each | doc comments |
| `linux/runtimes/.../MainActivity.java`, its manifest | 1 each | stale comment (`apps/<uid>`) |
| tests: `bwrap_test.c`, `install/tools/test-flatpak.sh`, `tests/runtime_review_test.py` | | new paths |
| docs: `STORAGE-LAYOUT.md`, `CODE-STORAGE-r24.md`, `README.md`s, `AGENTS.md` | | update |

`/data/matonos/linux/staging` in `file_contexts` has no code users left.
Check that, then delete the line.

## Steps

1. **One path header.** Add `linux/flatpak/linux-paths.h`, shared by linuxd
   and the helpers. It has `maton_user_of(uid)`, `maton_de_root(u)`,
   `maton_install_dir(uid)`, `maton_runtime_dir(u, runtime_uid)`,
   `maton_home_dir(uid)`, and a strict `maton_parse_install_path()` /
   `maton_parse_home_path()` that checks the user matches the uid. Do it
   first as a pure refactor (still old paths) so step 2 is a one-file diff.
2. **Switch the roots** in the header. Turn the `opendir(install)` scans into
   "for each numeric `/data/misc_de/<u>`, open `matonos-linux/install`".
3. **Directory creation and labels.** Patch `system/vold/vold_prepare_subdirs.cpp`
   (in `patches/system/`) to create `misc_de/<u>/matonos-linux` and
   `misc_ce/<u>/matonos-linux`; allow `vold_prepare_subdirs` to create and
   relabel to our root type. linuxd then creates
   `{install,runtime,store}` inside the DE root on the first request for that
   user, and `home` inside the CE root on first launch, with
   `fscreate` set (that also closes the "install/runtime trees need fscreate"
   gap in `STORAGE-LAYOUT.md`). Add the `file_contexts` entries, plus the
   linuxd/installer rules to search `misc_de`/`misc_ce` (still permissive,
   so the denials only show up as audit logs; they feed the gauntlet).
4. **Init/rc cleanup**: remove the moved mkdirs and restorecons.
5. **Helpers.** `build-flatpak-helpers.sh` rebuilds the wrapper and bwrap
   automatically. Also sync the headers to MatonOS_apexs
   (`stage-apex-helpers.sh` file list), because `FlatpakStore.c` and
   `app-session.h` are copied there.
6. **Tests/docs** as listed above.
7. **VM check**, on a fresh `/data`: install Firefox and Steam as user 0,
   then create a secondary user, install there, remove the user, and check
   that both trees are gone and linuxd keeps running.

## Open points

- The VM `/data` RAM disk (7.6 GB) is already too small for KDE runtimes. Fix
  it before step 7.
