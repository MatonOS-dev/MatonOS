# Handover 2026-11-06 — WSI spec fixes + JVM (TheoTown)

Repo: ~/matonos/repos/lsa-trim. Last commit 17164f0 (WIP checkpoint). EVERYTHING below is
UNCOMMITTED — commit only when the user asks. Installed on VM (localhost:5556), NOT yet tested.

## First thing tomorrow
Ask the user to restart Steam and test Big Picture + TheoTown on the installed build.

## In the installed build
- JVM PaX-probe EACCES: proxy SIGSYS handler stacks are now guarded (PROT_NONE page each side),
  so an RW anon page cannot merge with the handler stack VMA (kernel treats mprotect(PROT_EXEC) on a
  VMA containing SP as execstack → SELinux neverallow; device is ENFORCING).
  proxy.c guarded_stack_map/unmap + thread_state.signal_stack_guarded; arch/x86_64/thread-adopt.c.
- Adoption: arch/x86-filter.h:341 now `#ifdef MATON_GPU_BRIDGE` (x86_64 payload clones adopted too;
  was i386 only → adopt-miss-*). Sol report: out/pc-logs/agents/jvm-exec-1.last.md.
- Forwarder WSI (linux/vulkan-forward): per-image present semaphore+fence slots; acquire = import
  sync_fd -1 TEMPORARY (Mesa style); Wayland FIFO waits frame callback; multi-swapchain present;
  CONCURRENT sharing; 32-bit-class view formats + MUTABLE|EXTENDED_USAGE; present rects + device
  group queries; LINEAR dma-buf images answer vkGetImageSubresourceLayout (LINEAR kept: Zink/BPM).
  9/9 WSI tests pass.
- Compositor #6: configure_window returns bool; no-geometry configure requests are acked
  (compositor_core.c + new linux/subsystem/wlroots-xwm-configure-ack.patch).
  NOTE: build-subsystem.sh does NOT rebuild wlroots. Run build-compositor.sh manually:
  `MATON_AOSP_ROOT=/mnt/data/aosp MATON_COMPOSITOR_BUILD=$HOME/matonos/build/link-gc-out/deps/compositor bash linux/subsystem/build-compositor.sh`
- xsize_hints_test.py extractor accepts `static bool` functions.
- Earlier uncommitted: Xwayland fence publishing (xwayland-ahb.patch), Kopper readback,
  AHB mem-reqs fix.

## Open
- #5 (exported fd outlives freed memory): not done. No signal for the app closing its fd, so the
  options are the current drop-on-free or a leak. Recommendation: leave as is.
- #2 (PRESENT_SRC layout + FOREIGN ownership release/acquire): not done. It needs a submit at
  acquire, which partly undoes #7 and touches the BPM path; do it only after BPM is verified on
  this build.
- Strip TEMPDIAGs: wsi_x11.c logs, xwayland-present-diag.patch, mesa-zink-kopper-import-log.patch.
- Logcat of the last session: out/pc-logs/bpm-loop/lc11.txt (capture stopped).
