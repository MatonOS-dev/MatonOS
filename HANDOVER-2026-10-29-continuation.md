# Handover: 2026-10-29 continuation

Read this before the older chronological HANDOVER-2026-10-29.md. Source: `/mnt/data/aosp/out/pc-logs/agents/current session.txt` (previous agent transcript), with subsequent repository and device observations explicitly identified below.

## Current state

- Device-tree main: `02409a2`, matching local origin/main at inspection. Existing untracked files were left alone.
- Subsystem repository: `/home/hanro50/matonos/repos/linux-subsystem-app`.
- Subsystem main: `1dbec2a`, clean, two commits ahead of local origin/main. Contains I/O round 1 `2b0d733` and i386 routing fix `a304509`; the merge is local and unpushed.
- Existing agent VM: `localhost:5556`. User VM is `localhost:5555`; preserve it. Consult `/mnt/data/aosp/paths.md` for machine paths.
- I/O worktree `/home/hanro50/matonos/repos/lsa-perf`, branch `io-perf`, has additional unmerged commits `5b22241` and `dfab5fc`. These completed after the transcript ended. Do not reset or discard them.
- Steam worktree `/home/hanro50/matonos/repos/lsa-dns`, branch `steam-personality`, is at `a304509`.
- F-Droid implementation is merged in the device tree; device testing remains pending. Title bars have a design only and wait until Steam works.

## Read next

- [Steam and I/O technical handover](HANDOVER-2026-10-29-steam-io.md): fixes, measurements, pending branch, and next checks.
- [Software Centre and build handover](HANDOVER-2026-10-29-software-build.md): F-Droid, title bars, build recovery, and operating rules.
- Local evidence: `/mnt/data/aosp/out/pc-logs/day-r74/STATUS.txt` and captures beside it.

## Important correction to the transcript

The previous session left the Steam merge conflicted in `linux/compat-proxy/README.md`. Its shell continued after the merge failure, so the r73 build/install/capture did not validate `a304509`. Later continuation retained both README sections and completed merge `1dbec2a`.

A fresh build of `1dbec2a` finished with exit 0; log: `/tmp/maton-build-r61-codex.log`. The APK was installed on 5556. Visual inspection confirmed Kate and Obsidian. Chrome initially showed the launcher placeholder; on a 90-second retry it opened but its new tab showed `RESULT_CODE_INVALID_SANDBOX...`. Steam exited shortly after launch: process 13002 aborted with SIGABRT, a portal child exited 127, and the payload tree eventually reported exit 0. Cause is unresolved. Webhelper/UI logs cleared before launch remained empty; bootstrap_log.txt may be old. The five-minute wait was stopped after the process exited.

These observations do not establish that the i386 change caused either failure. Isolate against `2b0d733` before attributing a regression. No successful Steam main-window launch is established.

## User decisions to carry forward

- File I/O target: **at most 2× native per operation**, including read, write, openat, statx, access, readlink. Earlier 3–5× and 10× targets are superseded.
- Do not stop the VM or start Soong while critical Steam/i386 work is underway. Title bars wait until Steam works; F-Droid device testing waits for the next image.
- Priorities: Google-certifiable Android > performance > compatibility > simplicity. Fix compatibility generically, without per-app rules.
- Alpine's packaged Flatpak may be old. Build the pinned fork from sources.lock; do not chase output hashes as a substitute for source provenance.
- DeepSeek credits were replenished to the month's maximum; use sparingly as fallback. Transcript model quotas/reset times are historical, not current scheduling instructions.
- Old logs were authorized for deletion and deleted. This does not justify another blind deletion: build scratch was accidentally stored there and had to be recovered.

## Immediate next work

1. Preserve current branches and captures; inspect actual state before merging anything.
2. Isolate Chrome sandbox failure and Steam early exit with controlled builds of `2b0d733` and `1dbec2a`, using fresh logs and visual checks.
3. Review the two pending I/O commits, especially exact-fd seccomp filters, TSYNC, descriptor allocation changes, alias reuse, and mixed-ABI handoff. They are host-tested, not device-tested.
4. Build/install the reviewed result, validate the working apps and Steam, then run the committed device trap-cost recipe. Measure remaining path-operation overhead against the 2× target.
5. After Steam works, schedule the image build and F-Droid tests, then title-bar implementation.

This handover task changed documentation only. No new build, VM operation, merge, commit or push was performed while writing these documents.
