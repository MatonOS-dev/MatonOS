# Handover 2026-10-26 night (~23:15)

Read this first. Earlier context: `HANDOVER-2026-10-26.md` (architecture, recipes); day timeline in
`HANDOVER-2026-10-25-night.md` (status lines). Overnight run approved by the user for tonight (2026-10-26 → 27).

## State
- **Subsystem** main `8cd77ce` (pushed). **Device tree** main pushed. **Flatpak fork** `matonos/v26.10` at `8078df1`
  (reverts "skip apply_extra at deploy"), pinned in subsystem `build.sh`, `sources.lock`, `SOURCE`, README, FORKS.md.
- **VM**: disk VM (image v51, enforcing) running, adb serial **`localhost:5556`** (NOT 127.0.0.1). Subsystem APK
  `8cd77ce` installed. Logcat buffer raised to 64 MB (`logcat -G 64M`); always capture live (`logcat -v time > file &`).
- **Disk**: `/mnt/data` 40 GB free (96%). host-smoke output still at `out/pc-logs/host-smoke` (32 GB) — move to
  `/mnt/data2/matonos-host-smoke/` when no job uses it (none should now).
- **Worktree budget raised to 4** (user). In use: `lsa-trim`, `lsa-perf`, `lsa-dns`, `lsa-i386`.
- **Codex budget**: sol used 2/2 in the 21:15–23:00 window (chrome-sol r1+r2) → next sol job after ~02:15.
  User greenlit sol for Chrome if it keeps failing (memory `chrome-sol-greenlight`).

## Decisions today (all in memory)
- Extra-data apps (Chrome): subsystem pulls `--no-deploy` + verifies commit → installs MINIMAL stub (no image, no
  launcher) → binds stub's `ExtraDataInstallService` (result pipe over binder) → stub runs STOCK `flatpak install` into
  `<stub data>/install` (generic proxy hardlink emulation there) → stock deploy runs stock `apply_extra` → stub builds
  EROFS, streams back → subsystem `fsck.erofs` + verifies non-`/app/extra` files vs OSTree commit → 2nd PackageInstaller
  pass adds image/index/launcher splits → `install/` deleted. With EROFS the stub SHARES the installed runtime
  (read-only Flatpak system installation, runtime ref sent by the subsystem); without EROFS `install/` stays and is the
  deployment (launch from it = TODO). Isolated-process design is a dead end (memory `extra-data-stub-builds`).
- User rules: **stock Flatpak first** (emulate only what Android can't), **no overengineering**, no job caps (removed
  4-job caps in flatpak/compositor/xwayland builds), no arbitrary limits.

## App status (VM, main 8cd77ce)
| App | Furthest point | Current blocker | Fix waiting on branch |
|---|---|---|---|
| Chrome | stock `/app/bin/apply_extra` runs in bwrap | attempt 11: bind under `/etc/e2scrub.conf` (`/data/user/0` vs `/data/data` alias) | **chrome-sol `fb65a0d`** (+`0a02ea3` docs) |
| Kate | kate-bin forks GUI child | forked children killed by **SIGSYS** right after `adopt-fork-child` | kate-exit0 `9f12460` = test only, no fix |
| Obsidian | ldconfig OK, Flatpak seccomp denies userns probe | zygote `:207 Check failed EINVAL`; children hit **int3 (SIGTRAP)** after `adopt-fork-child`, signal-handler setup fails | obsidian-zypak `68515ea`+`f4048bd` (AF_UNIX io trace) |
| Steam | ldconfig step | `personality` returned -13 via a second dispatch path; ldconfig exits 1 (no stderr) | **steam-personality `204240c`** (one handler, both paths) |

**Key insight: Kate and Obsidian share one proxy bug — fork-child adoption (`adopt-fork-child`) leaves the child
unable to run (SIGSYS / int3, signal handler setup failing).** Fixing that is the highest-value item tonight.

## All four branches are finished, NOT merged
`chrome-sol` (lsa-trim), `steam-personality` (lsa-dns), `kate-exit0` (lsa-i386), `obsidian-zypak` (lsa-perf, already
rebased on 8cd77ce). Final reports: `out/pc-logs/agents/<name>.last.md`.

## Night plan (in order; park blockers and keep going)
1. Merge all four into subsystem main → full `./build.sh` (check rc before pushing!) → push → `adb install -r`.
2. Chrome attempt 12 via store UI (`tools/smoke-flatpak-app.sh "chrome" "Google Chrome"`, wrap in `timeout 1500` —
   the script keeps polling after a failure). Uninstall the leftover Chrome stub first
   (`flatpak.app.com_google_Chrome.x86_64.stable.xb1315e409eba`). grep logcat for `create-denied`, `v3 create backing`,
   `MatonExtraData`, `bwrap:`, `apply_extra script`. On failure → chrome-sol round 3 (sol after ~02:15, else luna-high).
   sol's `tests/chrome-sol/verify-device.sh` uses the debug trigger, which the store's caller check rejects — use the
   store UI path (or fix the trigger: LinuxAppStoreService.authenticateStubRef rejects the subsystem's own UID).
3. New job **fork-adopt** (luna-high, free worktree): proxy fork-child adoption bug from the Kate + Obsidian traces —
   `out/pc-logs/kate-exit0/kate-trace-lifecycle.txt` (pids 8952/8956, SIGSYS) and
   `out/pc-logs/obsidian-zypak/obsidian-trace-lifecycle.txt` (pids 9119/9121, int3, wait status 5). Give ABSOLUTE
   paths (kate-exit0 r3 said it couldn't find the trace). Then re-run Kate + Obsidian with `v3-trace`.
4. Obsidian unix-io trace (needs obsidian-zypak merged): `v3-trace` marker, look for `v3 unix-io` around the zygote boot.
5. Steam with `v3-trace`: check `v3 personality arg=`/`result=` lines and the ldconfig result.
6. Small open items: debug Chrome trigger caller check; launch from `install/` without EROFS (TODO); move host-smoke
   output to data2.

## Recipes / gotchas
- v3-trace for an app: `adb shell "touch /data/data/<pkg>/cache/v3-trace; chown <uid>:<uid> …"`, relaunch
  (`am force-stop` then `am start -n <pkg>/.Launcher`), remove marker after. Lifecycle lines: `lifecycle-`,
  `supervisor-descendant-status <pid> <raw wait status>` (31 = SIGSYS, 5 = SIGTRAP).
- Subsystem build: see `HANDOVER-2026-10-26.md`. Dependency rebuild (after fork pin change):
  `./build.sh --build-dependencies`, then `tools/refresh-flatpak-prebuilt-source.sh` and commit the binary + SOURCE.
- Never build while a VM test runs. Never `kill` via patterns; check `ps -o pid,ppid,cmd -p <pid>` first and pass a
  single PID (a multi-PID string fails silently).
- Codex: `cd out/pc-logs/agents && MATON_CODEX_EFFORT=high ./run-task.sh <name>` (brief `<name>.md`, append
  "Round N" sections to continue a job). sol: `MATON_CODEX_MODEL=gpt-6.1-sol MATON_CODEX_EFFORT=low`.
