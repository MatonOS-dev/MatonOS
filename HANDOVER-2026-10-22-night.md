# Handover 2026-10-22 night

## Stopped for the night
Nothing running: hourly cron deleted, agent glx-caps2 stopped (literal PIDs), disk VM 5556 stopped (synced), VM 5557
was stopped by its agent. No watchers left.

## Done today
- **Corrupt screenshots/readback (handover 10-21 item 1) fixed — host-side.** The 10-20 qemu-venus virglrenderer was
  built with `-Dminigbm_allocation=true`; that breaks GL import of every gralloc AHB (white/garbage screencap, broken
  Xlib/GLX readback) on venus AND virgl. Rebuilt with it off (venus-import patches kept). Bisect recipe + test tool in
  memory `screencap-corruption-2026-10-22`. Fork now public: https://github.com/MatonOS-dev/virglrenderer
  (`matonos/venus-import`); build line recorded in `~/matonos/qemu-venus/build.sh` and FORKS.md.
- **Extreme Tux Racer renders correctly** under v3 GLX (blending, text, frost): glx-blend fix (shared GLES/desktop
  names routed to GLES thunks in a desktop context → glvnd stub jumped to 0; also EXT/ARB aliases).
  Screenshot `/home/hanro50/matonos/build/glx-blend-out/tux-racer.png`.
- **install-plan**: extension parent can come from installed set; dependency repair follows recorded origins.
  GNOME Calculator installs + launches (with Platform 26.08 preinstalled).
- Pushed: linux-subsystem-app `main` (= `egl-window` on GitHub) at 8d7922a; device tree main (df12467 + this commit).
- Adversarial sweeps (free DeepSeek): `out/pc-logs/agents/sweep-system.final.md`, `sweep-gpu.final.md`.
- sweep-fixes (codex): 29 commits on `egl-window` up to **eae6fc0 — LOCAL, not merged to main, not pushed.**
  alpha.c probe verified by me on that build (all pass); Tux Racer still renders.

## Open — resume here
1. **glx-caps** (`lsa-gpuint`, uncommitted `linux/glx-forward/vendor.c` diff on eae6fc0): sweep commit 4182256 wrongly
   downgraded GLX to 1.2 and dropped ARB_create_context(+profile)/swap-control — breaks SDL2/GLFW/core-profile apps.
   Partial fix restores 1.4 + extensions with attribute→EGL mapping. Still to do (brief `agents/glx-caps2.md`, relaunch
   it): host tests, core 3.3 VM triangle test, alpha.c, Tux Racer check, full build, commits; swap interval does not
   pace the CPU-readback/XPutImage present path yet. Then merge egl-window → main and push.
2. Deferred sweep items needing design choices (bring options to user): sys F1 EGL surface-list lock without
   callback deadlock, sys F3 stopped-window swap wake-up, sys F6 deadline-aware Vulkan acquire, sys F11 replace 10 ms
   AF_INET poll with a wake-up, gpu F10 EGL extension allowlist.
3. Calculator gaps: installing an app whose runtime isn't installed still fails (planner must pull the runtime —
   "Flatpak resolves deps"); runtime-stub repair verified only by JVM test.
4. v44 image (Mesa fork, xperm rule) → regenerate disk VM → enforcing boot. User question still open: build v44 before
   or after the sweep-fix batch lands (VM must be stopped for the image build).
5. Clear ~/.gradle when no Gradle daemon runs (16 GB).

## Order (user, 10-22)
Flatpak working first (above), then OTA update server + installer, then real PC. Memory `roadmap-order-2026-10-22`.

## Notes
- VM recipe: `MATON_QEMU_HEADLESS=1 MATON_QEMU_DISK=nvme MATON_QEMU_PERSISTENT=1 MATON_QEMU=~/matonos/qemu-venus/bin/qemu-system-x86_64 tools/run-qemu-live.sh -i ~/matonos/vm/disk-vm/disk.img -g venus -m 12288 -a 5556 -s ~/matonos/vm/disk-vm/serial.log`.
  Avoid `adb wait-for-device` after `adb root` over TCP (hangs); `sync` after pushes before killing the VM.
- Probe binaries run in the Tux Racer stub via `out/pc-logs/glx-shim3/run.py <binary>`; output in logcat tag MatonV3.
- "Sweep" = adversarial, report-only (memory `sweeps-adversarial`).
