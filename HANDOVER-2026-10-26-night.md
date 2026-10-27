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
- 23:15 night start: hourly check cron 5b951aa0 (:13). Merged chrome-sol, steam-personality, kate-exit0, obsidian-zypak → subsystem main ab4f939 (clean); full build.sh running.
- 23:30 build rc=0, pushed ab4f939, APK installed. Chrome a12: alias bind fixed; new blocker `bwrap: execvp /app/bin/apply_extra: Permission denied` (out/pc-logs/chrome-a12). Launched chrome-sol r3 (luna-high, lsa-trim) + fork-adopt (luna-high, lsa-i386). VM idle until they report.
- 00:30 chrome-sol r3 (59d96a4 noexec emulated exec) + fork-adopt (bf66699 chain fork-child SIGSYS) merged → main 244d799, build rc=0, pushed, installed. Chrome a13 running (out/pc-logs/chrome-a13); Kate/Obsidian v3-trace re-runs next.
- 00:45 Chrome a13: same execvp EACCES, no `v3 exec resolved` → 59d96a4 path not reached; round 4 → sol after 02:15. VM traces (out/pc-logs/fork-adopt-vm): Kate still SIGSYS on clone 4113 child; Obsidian int3 gone, children now exit 127; Steam personality still -13 via ours. Launched fork-adopt r2 + steam-personality r2 (luna-high).
- 01:45 merged fork-adopt r2 (be0c56b) + steam-personality r2 (a707213) → main 091429f, rc=0, pushed, installed. Traces out/pc-logs/night-r3: Kate SIGSYS persists, Obsidian exit 133, Steam ldconfig exit 1; all adopted children log adopt-fork-altstack-install -1 → fork-adopt r3 (luna-high) running. Chrome r4 sol at 02:17.
- 02:17 Chrome r4 launched on sol-6.1-low (lsa-trim rebased on main; sol 1/2 this window).
- 02:45 merged fork-adopt r3 (d2de7fa) + chrome-sol r4 (9c55e20, 1724579) → main 3357832, rc=0, pushed, installed. REGRESSION (out/pc-logs/night-r4): Kate main proc SIGSYS after mprotect, Obsidian/Steam ldconfig SIGSYS. fork-adopt r4 on sol (2/2) with bisect. Chrome a14 waits for that fix (same proxy).
- 03:40 merged fork-adopt r4 sol (d30d563: altstack via raw gateway; bisect: d2de7fa caused nested SIGSYS) → main, rc=0, pushed, installed. night-r5: regression gone; Kate clone(4113) child still SIGSYS (parked → sol r5 at 07:19), Obsidian children 127/133 → new job obsidian-exit127 (luna-high, lsa-perf), Steam ldconfig exit 1 (parked for sol slot 2). Chrome a14 next VM slot.
- 04:40 obsidian-exit127: no fix (trace ids are proxy op ids; 127 child dup2(15→4) EBADF — fd 15 gone after adopt; needs per-pid dup2/execve trace). Chrome a14: apply_extra RUNS + stub mkfs OK; now fsck.erofs in stub fails silently → chrome-sol r5 (luna-high). Kate+Steam parked for sol 07:19.
- 05:50 merged ee9bdf5 (fsck exit status), rc=0, pushed. Chrome a15: stub fsck.erofs killed by SIGSYS (Android app seccomp, run outside proxy) → chrome-sol r6 (luna-high).
- 06:55 merged a4d2bef (fsck skip utimensat), rc=0, pushed. Chrome a16: fsck STILL SIGSYS (binary verified in APK) → chrome-sol r7 (luna-high): check all raw syscalls vs real app policy. Kate/Steam sol at 07:19.
- 07:20 sol window: fork-adopt r5 (Kate vfork child + Obsidian fd 15) and steam-personality r3 (ldconfig exit 1) both on sol-6.1-low (2/2). chrome-sol r7 luna-high still running.
- 07:45 merged chrome-sol r7 (35c6d92 fsck *at calls, generated bionic app policy test) → main, pushed. NOTE: default ./build.sh FAILS (rc=1) in fsck-app-seccomp-host.py: build.sh sets MATON_AOSP_ROOT to the build context (no bionic); passes (rc=0) with MATON_AOSP_ROOT=/mnt/data/aosp — use that until fixed. Chrome a17 waits until the sol jobs finish (no VM during builds).
- 08:55 merged fork-adopt sol r5 (5e41957) + steam sol r3 (8857007) → main 6e93f6c, rc=0 (with MATON_AOSP_ROOT), pushed. **KATE WINDOW RENDERS** (night-r6/kate.png). Steam past ldconfig → 'flatpak run: … not installed'. Obsidian: dup2 target fd 4 is proxy-private. Chrome a17: fsck SIGSYS fixed → hard link extraction denied (untrusted_app link). Launched chrome-sol r8, fork-adopt r6 (Obsidian), steam-personality r4 — all luna-high.
- 10:00 merged chrome r8 (21cb067 hardlinks), fork-adopt r6 (897231e dup2 over resident fds), steam r4 (da88806 deployed refs) + my f4b982c (Files.writeString not on Android) → main, rc=0, pushed. Kate still renders. **STEAM WINDOW OPENS** (black, polling; night-r7/steam.png). Obsidian: zypak 'Failed to dup 32750' (reserve fd) → exit 133 → fork-adopt r7. Chrome a18: fsck OK, validator 'EROFS mode differs from commit: /app' → chrome-sol r9.
- 10:40 merged 64f3d1a, rc=0, pushed. Chrome a19: /app ok; '/app/manifest-base-1.json expected 1600, actual 0644' (commit mode misparse) → chrome-sol r10. fork-adopt r7: no fix (needs VM trace of fd 32750 origin).
- 11:35 Chrome root cause: stub image builder reads install/ raw → 385 proxy hardlink markers (01600, links.c) go into the image → chrome-sol r11 (luna-high).
- 11:25 check: chrome-sol r11 running (just started, existing hardlink-store Java helper found). VM idle (no tests during codex build). Queue after r11: Chrome a20 → Obsidian fd-32750 trace → Steam long run.
- 12:50 merged chrome r11 (4d93cce) + my marker fix in ExtraDataInstallService → main, rc=0, pushed. Chrome a21: **stub validation passes, 290 MB EROFS handed back**; subsystem fails 'The authorized application was not deployed' (DeployManifest:44) → chrome-sol r12.
- 12:25 check: chrome-sol r12 running (just started). VM idle. Queue after: Chrome a22 → Obsidian fd-32750 trace → Steam long run.
- 12:45 user: app builds OK with VM, only image builds conflict (memory updated) → VM testing alongside codex now. Obsidian: app sees RLIMIT_NOFILE 32768, zypak dup2s near top (32750) into proxy reserve → fork-adopt r8. Steam (no trace): SIGSEGV = handler-stack overflow in links.c tree() after pivot_root → steam-personality r5. chrome-sol r12 running.
- 13:40 merged chrome r12 (13f98f3 no-deploy extra-data deployment), fork-adopt r8 (7e60a39 fd dup in reserve), steam r5 (c5d3118 bounded hardlink stack) → main 1e78b99, rc=0, pushed. day-r9: Kate OK; Obsidian no longer crashes (running, no window yet); Steam no SIGSEGV, black window 4 min (one child SIGABRT in libc). Chrome a22 running.
- 13:55 Chrome a22: subsystem validation 'EROFS image contains uncommitted path: /app/include' → chrome-sol r13.
