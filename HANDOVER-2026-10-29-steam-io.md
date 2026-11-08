# Steam and file I/O handover — 2026-10-29

Current entry point: [continuation handover](HANDOVER-2026-10-29-continuation.md). Historical source is `/mnt/data/aosp/out/pc-logs/agents/current session.txt`. Round numbers identify investigations, not releases.

## Steam: what was fixed

| Round / commit | Result and limits |
| --- | --- |
| r45 / `510028f` | Marker cache survives file changes; resident adoption restricted. Host probes 3,588 → 11, but device setup remained slow. |
| r46 / `4467eeb` | Fewer symlink checks and correct absolute /proc/self/exe; no observed Steam fix. |
| r47 / `9b69d60` | Diagnostics caused pv-adverb locale-child segfaults. Reverted by `96e95c0`; preserved on `steam-diag-r47-rejected`. Do not restore. r48 docs-only `65b1c56` was left recoverable via reflog. |
| r49 / `76a115d` | Child-status diagnostics established zygote deaths from SIGPIPE, causing GPU launch error 1002. |
| r50 / `1aa47da` | Closed diagnostic-pipe SIGPIPE fixed. SIG_IGN already survived fork/exec; this was not the Steam cause. |
| r51 / `5c0fef5` | Diagnostics broke pressure-vessel startup; reverted and preserved on `steam-diag-r51-rejected`. |
| r52 / `e904326` | Corrected F_DUPFD resident-reader ownership; Steam still failed. |
| r53 / `e3eb3c7` | Internal proxy writes made SIGPIPE-safe; Steam still failed. |
| r54 / `34ffb53` | Android liblog redirected through proxy-owned protected logd client with MSG_NOSIGNAL. Device GPU launch errors dropped to zero. |
| r55 / `7f6f936` | Vulkan XCB/Xlib surfaces via DRI3/Present; missing VK_KHR_xcb_surface error resolved. |
| r56 / `c0bf466` | Real X11 visuals in GLX path; conditional Vulkan swapchain advertisement. Invalid visual ID error resolved. |
| r57 / `9097b38` | GLX pbuffer and ES context support. Historical five-minute capture had no GPU exits/init errors, but no Steam UI. |
| r58 / `ecad826` | Size-hint loop reduced from 12,908 reapplications to one; operation counters added. Timeout dialog became visible in later run. |
| r59 / `c6bbac2` | Hard-link emulation optimized; startup proxy counter time reported 67.5 s → 6.2 s. Different runs/stages mean this is not an isolated end-to-end benchmark. |
| r60 / `3271d90`, `ba076fb` | ENOTTY for terminal probes on non-character fds, rewritten-call counters, Android bounds following fixed X11 hints. Final dialog size still needed visual verification. |
| `b3787de` | Routed terminal probes to handler, but device i386 TCGETS storm persisted. |
| r61 / `a304509` | Fixed i386 AT_SYSINFO fallback bypass. Host regression fails before and passes after; device benefit remains unverified. |

## Proven SIGPIPE cause

Kernel ftrace in `out/pc-logs/day-r62/ftrace4.txt` linked fatal SIGPIPE to bionic writev(fd 5, four iovecs) returning EPIPE. Chromium's inherited-fd sweep closed/reused liblog's socket while SIGPIPE was at SIG_DFL. Android-side writes ran in the exempt native range and were invisible even with payload syscall rewriting disabled. The r54 logging redirect fixed the GPU-launch failure on the historical device run.

Useful diagnostic technique: `/sys/kernel/tracing`, `signal/signal_generate` filtered with `sig == 13`, `options/userstacktrace`, plus syscall enter/exit writev events. Capture process maps in the same run: ASLR makes addresses from other sessions misleading. Keep signal-handler diagnostics small; r47 and r51 both broke startup.

## Remaining i386 and CEF questions

r70 had 233,024 AVC lines; 229,605 were ioctl on `/memfd:v3-mountinfo`. r71 still had 211,245. Command was TCGETS (0x5401), mostly the i386 client's CSteamControlle and SteamUdevContex threads. Merely adding the ioctl predicate did not work because AT_SYSINFO executed in exempt bionic code.

`a304509` gives payload fallback syscalls a separate RX `matonos-i386-sysinfo-trap` page outside native exemptions. With rewriting on, the direct entry tail-calls it for fallback; native bionic retains its startup trampoline. The production-peer test uses an inherited kernel TCGETS/EACCES filter: synthesized mountinfo must return ENOTTY, while /dev/null retains EACCES. Both rewrite settings pass. Full suite and preflight were reported passing.

The r61 report also corrects the earlier claim of a permanent pressure-vessel stall: r70 process snapshots show a live CEF browser, GPU and zygotes at 180 and 240 seconds. Poll/futex/stdin waits alone do not prove deadlock. Logs contain kernel rendering error 13 and ANGLE glTexImage2D GL_OUT_OF_MEMORY (0x505). These are leads, not proof of the init-handshake timeout's cause.

Full r61 report: `/mnt/data/aosp/out/pc-logs/agents/steam-personality.deepseek.md`. Capture helper: `/mnt/data/aosp/out/pc-logs/agents/steam-r60-procs.sh`. New continuation failures and evidence are described in the entry handover and `day-r74/STATUS.txt`.

## File I/O round 1 — merged

`2b0d733` routes ordinary pread64/pwrite64/writev natively, removes redundant marker metadata probes, reuses translation caches, and avoids repeat component walks after ENOENT/ENOTDIR. Mutable-tree negative translations are deliberately not cached: another process can create an absolute symlink crossing a virtual mount.

Host median µs/op (native / before / after): read 0.625 / 4.264 / 4.327; write 0.759 / 4.615 / 4.630; openat 2.189 / 17.430 / 12.614; statx 1.215 / 10.492 / 8.952; access 0.996 / 13.129 / 8.456; readlink 1.088 / 12.605 / 7.677. All missed 2×. Positive path operations improved 15–39%. Full suite reported passing.

Historical Steam counters showed openat ~305 µs, read ~199 µs, statx ~96 µs, access ~241 µs, readlink ~111 µs; executable checksum verification took 43 s. These are aggregate device observations, not paired native/proxy microbenchmarks, and do not identify the cause of the host/device gap. recvmsg was another lead: 43,708 calls, ~4 s total.

## File I/O round 2 — completed after transcript, unmerged

Worktree `/home/hanro50/matonos/repos/lsa-perf`, branch `io-perf`:

- `5b22241`: fd-specific sequential I/O routing.
- `dfab5fc`: device trap-cost probes, profiling corrections, remaining gaps.
- Report: `linux/compat-proxy/IO-PERF.md` in that worktree; main's copy still describes round 1.
- Device recipe: `linux/compat-proxy/tests/DEVICE-IO-PERF.md`.
- Evidence: `/mnt/data/aosp/out/pc-logs/io-perf-round2/`; final report: `agents/io-perf.last.md`.

| Operation | Native µs | Round-1 baseline µs | Round-2 µs | Ratio |
| --- | ---: | ---: | ---: | ---: |
| read | 0.624 | 4.491 | 0.728 | 1.17× |
| write | 0.768 | 4.710 | 0.987 | 1.29× |
| openat | 2.209 | 13.180 | 12.869 | 5.83× |
| statx | 1.217 | 9.300 | 9.193 | 7.55× |
| access | 0.993 | 8.918 | 8.813 | 8.88× |
| readlink | 1.076 | 7.741 | 7.643 | 7.10× |

These paired host medians cover five interleaved runs, 30,000 operations per row, a warm EROFS-like mapped directory tree, not a mounted EROFS image. Ordinary read/write meet the target in this workload; overall target remains unmet. Negative path lookups remain roughly 8.6–12.4× native. No device performance claim is justified yet.

Special signalfd, inotify and writable virtual sysctl fds move to an I/O window, usually [512,1024), below fd_set's limit. Ordinary read/write/readv outside that and the private window bypass SIGSYS. Explicit aliases outside the window install exact-fd seccomp routing with TSYNC. dup2/dup3 target numbers and lowest-free dup/F_DUPFD semantics are retained; initial special-fd allocation deliberately changes return numbers. Full-window/reduced-RLIMIT cases fall back to low fds plus exact filters.

Review limitations carefully: exact filters cannot be removed, so an ordinary file reusing a former alias still traps; ordinary fds in reserved windows trap; many alias numbers grow the filter chain; TSYNC failure handling and actual device behavior need validation. Emulation must retain forwarded SIGCHLD decoding, split signalfd readv, inotify EOF workaround, sysctl synchronization, SCM_RIGHTS, inherited descriptors, mixed ABI and exec handoff.

Reported validation: full suite exit 0, 231 PASS lines; static probes and profiler checks pass. Host perf sampling was unavailable. Minimal SIGSYS probe measured 2.111 µs versus native getppid 0.270 µs; proxy emulated ioctl ~3.526 µs. These are separate probes, not additive phase timings. Path operations still pay SIGSYS and translation; caching alone cannot meet a ~2 µs budget with that trap cost.

Run the committed device recipe only after reviewing it, selecting a disposable installed stub and preserving/restoring its overrides. It measures the installed proxy, so install the intended revision first. Shell-native numbers have a different UID/SELinux/seccomp context and are not a matched stub baseline.

## Validation discipline

Clear relevant Steam logs before each run; Steam appends. Empty logs can mean startup failed before CEF, not success. Check disk space and command exit codes; never let a failed merge/build proceed to installation. Inspect screenshots, not pixel counts: a launcher placeholder has plenty of non-background pixels. Keep count-only profiling separate from full tracing, and confirm intended ABI binaries were rebuilt. Record exact revision, APK, run timestamps and evidence. Do not interpret syscall IDs without `arch/syscalls.h`: proxy operation IDs differ from Linux syscall numbers.
