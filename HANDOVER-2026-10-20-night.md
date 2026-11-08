# Handover 2026-10-20 night

## State
- Images: v42 (13:06) and v43 (21:15) OK. v43 = v42 + misc rule for matonos_driver. Copies: ~/matonos/vm/coord/v4{1,2,3}.img.
- Device tree main: 764c3f4 (local, NOT pushed) = misc rule + `allowxperm ... ioctl 0x1280` (BLKGETDISKSEQ) +
  `MATON_QEMU_DISK=nvme` in tools/run-qemu-live.sh. The xperm rule is not in any image yet (needs v44).
  origin/main = c2f5e3b (disk-vm installer work, pushed today).
- minigbm matonos/v26.10 = d6e1d11 (revert of ineffective c56630c, pushed).
- Nothing running: no builds, VMs or codex agents.

## Disk-backed VM (item 6): works
- Disk: ~/matonos/vm/disk-vm/disk.img, built from v43 with docs/DISK-VM.md. Its slot-a UKI is currently a
  hand-signed DEBUG kernel (verbose + androidboot.selinux=permissive). Regenerate the disk from v44 before trusting it.
- Boot (always headless; a GTK window freezes the guest while the screen is locked):
  `MATON_QEMU_HEADLESS=1 MATON_QEMU_DISK=nvme MATON_QEMU_PERSISTENT=1 MATON_QEMU=~/matonos/qemu-venus/bin/qemu-system-x86_64 tools/run-qemu-live.sh -i ~/matonos/vm/disk-vm/disk.img -g venus -m 12288 -a 5556 -s ~/matonos/vm/disk-vm/serial.log`
- Verified (permissive): boot 20–25 s, /data on nvme0n1p5, file persists across a VM restart.
- Enforcing failure on v43 was only BLKGETDISKSEQ → 'Checkpoint commit failed' reboot ~8 s (fixed in 764c3f4).
- Fresh installs fill only slot a; slot b cannot boot (expected; installer could say so).

## GL (item 4): blocked on a GL.maton branch bug
- On the disk VM: subsystem APK (main-out, egl-window build) + Tux Racer installed (13 min, no LMK).
- Tux Racer launch fails: `Maton GL extension branch 26.08 is not supported by the runtime GL extension point`
  (runtime Platform 25.08; gradle hard-codes --branch 26.08). Codex task gl-branch (brief
  out/pc-logs/agents/gl-branch.md, worktree ~/matonos/repos/lsa-gpuint, egl-window) was stopped at shutdown with no
  changes. Decisions in the brief: branch = runtime's own branch if listed else first listed; generate the GVariant
  deploy record at launch with a small Java writer (byte-identical to build.py for 26.08). Relaunch it.

## Vulkan AHB (item 3): user chose virglrenderer patch (option 2)
- virglrenderer ~/matonos/qemu-venus/src/virglrenderer branch matonos/venus-import: c3689a7 + 6d2a073 (venus imports
  attached virgl resources via dma-buf). Host QEMU/virglrenderer rebuilt with -Dminigbm_allocation=true; old install
  backed up in ~/matonos/qemu-venus/backup-2026-10-20. Boot works; import NOT tested yet.
- minigbm worktree out/worktrees/minigbm-venus-blob branch venus-host: f1274d4 (host-visible blobs with Venus).
- Probe binary: ~/matonos/vm/venus-host/ahb-native-probe (NDK 30, API 34). Test: bind-mount venus-host minigbm libs
  over /vendor in `unshare -m` (rprivate first), run probe, check `journalctl --user` for "invalid res_id".
- If it passes: create a MatonOS-dev virglrenderer fork (user approved), push, add to FORKS.md; merge f1274d4.

## Next
1. Relaunch gl-branch codex → install APK on disk VM → Tux Racer GL test (SDL Wayland vs X11, EGL window). Merge
   egl-window if GL works.
2. Build v44 (coord-build.sh -K -M, no VM during the build) → regenerate disk → enforcing boot check → push main.
3. Vulkan probe test (above).
4. Store service: progress/RESULT callbacks go to frozen clients (test installer never gets RESULT).
5. Handover items 5 (cheaper proxy fixes) and 7 (re-pin bootstrap APK).

## Lessons today
- Locked screen + GTK window = guest nearly frozen (lost the afternoon). Memory: vm-headless-when-locked.
- 15:14 accidental `coord-build.sh --help` started a bogus build. coord-build.sh has no --help.
- 17:34 build died: host zram decompression failure (kernel WARNING zram_bio_read) → siso SIGBUS, under memory
  pressure from codex builds + VM. siso saves state only at the end of a build, so the rerun redid ~90k steps (3 h).
  If zram errors recur: memtest. Never run VMs/agent builds alongside an image build.
- The quick path keeps being refused for unknown reasons; coord-build.sh now saves the refused log to
  out/pc-logs/quick-refused.log — read it on the next build.
- Build watcher: match `^date time (OK|FAILED|KILLED) ` on the last timestamped line (warnings get appended).
