# Handover 2026-11-05 night: Steam webhelper exit, soundtrack loop, UI spin

Read this first next session. Tree: `~/matonos/repos/lsa-trim`. Everything below is UNCOMMITTED.
The sol report with full evidence is `out/pc-logs/agents/steam-all.report.md` (brief: `steam-all.md`).

## State right now
- The last build/install on the VM does NOT contain the two newest diagnostics (see "Unbuilt" below).
- The source has the misleading-indentation build error already fixed (the sed ran even though
  the call was rejected). The build itself never ran.
- Build:
  `MATON_SUBSYSTEM_REPO=$HOME/matonos/repos/lsa-trim MATON_SUBSYSTEM_OUT=$HOME/matonos/build/link-gc-out /mnt/data/aosp/device/maton/pc_x86_64/tools/build-subsystem.sh`
  Then install with `adb -s localhost:5556 install -r --no-incremental $HOME/matonos/build/link-gc-out/MatonLinuxSubsystem.apk`.
  "error: Repository corruption encountered" in the build log is an intentional negative test.
- build-subsystem.sh does NOT rebuild Xwayland. Run
  `MATON_AOSP_ROOT=/mnt/data/aosp MATON_COMPOSITOR_BUILD=$HOME/matonos/build/link-gc-out/deps/compositor bash linux/subsystem/build-xwayland.sh`
  first whenever its patches change.

## Findings so far
1. **Webhelper exit(1) on a new GL window** (toast 283x70, Big Picture, a new 1280x800 window).
   - The webhelper exits first and Xwayland closes its clients after it (CloseDownClient log:
     all exc=0). Xwayland is not the cause.
   - The caller of exit(1) is still unknown. The earlier exit-frame dump never ran for x86_64: the
     adoption handler `arch/x86_64/thread-adopt.c` exit_group arm bypassed it.
   - Suspects: an Xlib/XCB I/O error during window setup, or a race in the X11 WSI shared event
     queue (`vulkan-forward/wsi_x11.c:213-240`).
2. **Soundtrack loops its first second.** The AAudio bridge is steady (48 kHz, no replay path).
   The audio producer (likely the i386 client: sound keeps playing after the webhelper dies) is
   not refilling, or is reusing a buffer. Next step: a per-stream server log (owner pid,
   sequence, PCM checksum). NOT written yet.
3. **UI freeze / i386 spin.**
   - IPC:CSteamEngine loops on `poll(fd,1,0)` + clock_gettime64. The fd's identity is unknown.
   - CSteamController rereads mountinfo. Our mountinfo is a fresh memfd on every open, with mount
     ID 1 on every row (`vfs.c:1238-1251`): a real semantic gap, but its link to the spin is
     unproved.
   - The TCGETS ENOTTY fix is already in the WIP (`proxy.c` ~2600).
4. **hardwareupdater --check-for-updates exits 255 every ~3 s.** The child gets 3 execstack
   denials, then exit_group(-1). It very likely needs an executable stack. Which ELF requests it
   is not yet identified.

## Unbuilt diagnostics added tonight (TEMPDIAG)
- `proxy_exit_frames` is now non-static (proxy.h) and is called from the x86_64 adoption
  exit_group arm with `g[REG_RSP]` (thread-adopt.c ~588). This covers findings 1 and 4.
- `poll0_diag()` in proxy.c, called from `PROXY_SC_poll` when the timeout is 0: once per second
  it logs `v3 trap poll0 fd/events`, `poll0 ret/revents` and `v3 poll0 target=`. This covers
  finding 3. Note: it only hooks `poll`, not `ppoll`; the i386 engine thread used poll (NR 168).
- Plan: build + install, reproduce once (Big Picture or toast, play a soundtrack, wait for the
  spin), grep logcat for `exit frame=`, `poll0`.

## Temporary diagnostics to remove once done
- The Xwayland close log: `linux/subsystem/xwayland-close-log.patch` plus its block in build-xwayland.sh.
- Xwayland `-verbose 7` and the stderr→logcat redirect in xwayland-launch.c (keep the logcat part if useful).
- The audio `diag` counters in `subsystem/native/pipewire_android.c`.
- `proxy_exit_frames`, `poll0_diag`, and the TEMPDIAG stack dump at proxy.c ~199.

## Other WIP in the same tree (keep, commit when asked)
- links.c: dirmove store lock and known_objects validation.
- vfs_free arena pop, sysvipc LIFO frees.
- wsi_x11.c: 64 px rounding removed.
- package-xwayland.py: `-llog`.

## Logs
- `out/pc-logs/bpm-loop/`:
  - lc3/lc4/lc5.txt: full logcat. The lc5 webhelper exit is at line ~273307 (pid 26390, 21:00:41 VM time).
  - audio.txt, play.txt.
- Earlier logs: steam-crash2/, chrome-alert/. Reports: agents/xwl-reset.report.md, agents/steam-all.report.md.
- The VM clock is ~2 h behind the host.

## Rules reminder
- No strace/gdb on Steam (simpleperf and ftrace are fine).
- Kill literal PIDs only.
- No Soong while the VM runs.
- Investigation briefs are report-only.
