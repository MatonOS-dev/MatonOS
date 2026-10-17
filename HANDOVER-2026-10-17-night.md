# Handover 2026-10-17 (night)

Read after `HANDOVER-2026-10-17.md`. Agent briefs/logs/reports: `out/pc-logs/agents/<name>.{md,log,last.md}`.

## Decisions today (user)
- **Cost rule:** anything desktop Linux also pays counts as 0; report/optimise only our delta.
- **Buffer transport:** clients send their own AHBs to the compositor (`maton_ahb_buffer_v1`,
  `AHardwareBuffer_sendHandleToUnixSocket`, once per buffer; per frame attach/damage/commit + sync_file).
  Still dead: compositor allocating AHBs and pushing them to clients (DEAD-ENDS.md, 45ca628).
- **linux-dmabuf + minigbm patch + `dmabuf_import.c` go away** once the AHB path is verified on the VM,
  Xwayland uses AHBs, and no client sends raw dma-bufs.
- **GL:** EGL + GLES (and desktop GL if Android offers it) pass through to Android via our glvnd EGL
  vendor `libEGL_maton.so`. **Zink is fallback only** (runtime Mesa behind our vendor in glvnd order).
  X11 apps use EGL (X11/XCB platform) through our vendor; GLX later as GLX→EGL shim.
- **Xwayland:** Android's own libEGL/GLESv2, glamor on AHBs, presented over `maton_ahb_buffer_v1`.
  Xwayland is a **separate process** (wlroots spawns it) — not in-process (my earlier claim was wrong).
- Two agent VMs are fine (each `-m 4096`): ports 5556 and 5558.

## State
- **Subsystem main = `73069e0`** (pushed):
  - `0fbaca3` Vulkan Wayland WSI (swapchain, sync_file present, AHB wl_buffers) — host-tested, not VM-tested.
  - `065d425` + `6b208f1` packing reads OSTree metadata in-process (xattr for bare-user, object file stat for
    bare-user-only = device repos). Fixed the "packs 90%" hang (one ostree process per file, ~1 s each);
    pack phase now ~0.26 s on the VM.
  - `perf-fixes-1` (`0f4f38a..9c92d92` + my `59edb82`: luna's spool "fix" was more I/O; reverted to
    streaming double-read) and `perf-fixes-2` (`bb33687..3e8d468`: index parse once, icon lookup, DNS
    eventfd poll, launch-status event).
  - `73069e0` compat proxy logs `v3 trap fatal-sigsys <kernel nr> <si_code>` + `fatal-sigsys-pc`, and
    `v3 trap unhandled <kernel nr> <id>` once per ENOSYS fallthrough (no tombstone exists for these procs).
- **Device tree main = `5c56c5a`** (pushed): enforcing fixes `845a6af` (priv_app_all socket), `0ddefb5`
  (IChannel/status label, udmabuf for apps), `5c56c5a` (allocator HAL attribute, system_server open/ioctl).
  **Not in any image yet.** Subsystem pin is still the old `72d6bac` pin.
- **lsa-gpuint worktree:** branch `gpu-integration` (`ed51168`, GPU bridge in production loader + launches,
  stub APK fixes; NOT merged to main). Branch `egl-forward` = `89d3fb2` (offscreen EGL/GLES forwarder 751071c
  + APK/V3Launcher packaging, committed by me from egl-forward-1b's leftovers). Branch `xwayland-gles` checked
  out there by the running Xwayland agent.
- **lsa-vkwsi worktree:** branch `sigsys-fix` = main + gpu-integration (`ae3bb21`).

## Running at handover (left running)
- **sigsys-159** (luna-high, VM 5558, `~/matonos/vm/sigsys/`): find the SIGSYS behind vulkaninfo's
  "Loader exited: 159" (= 128+SIGSYS, likely bwrap passing up its child's signal). Uses the new proxy logging.
- **xwayland-android-gles-2** (luna-high, VM 5556, `~/matonos/vm/xwl/`): Xwayland on Android EGL + AHBs over
  `maton_ahb_buffer_v1`.

## Blocker → first thing tomorrow
Both VM tests run on image `v35c`, which predates the policy fixes. egl-forward-1b failed installing a
test app on the known `forwarder.udp` socket denial (`priv_app` → `privapp_data_file`, enforcing).
**Recommended (awaiting user):** stop the agents (literal PIDs + codex children), merge `gpu-integration` +
`egl-forward` into main (suites on the merge commit), pin subsystem main into the device tree, preflight,
one image build (`coord-build.sh` kills test VMs), then restart sigsys / egl acceptance / xwayland on it.

## Next, in order
1. New image as above; boot on home disk `-m 12288 -a 5556`.
2. SIGSYS fix → `vulkaninfo --summary` in a Flatpak via the stub path.
3. EGL step 1 acceptance (pbuffer readback glibc+musl, `eglinfo` in Flatpak); step 2 Wayland + X11 surfaces;
   step 3 Zink fallback.
4. vkcube windowed (first VM test of AHB wl_buffers), Firefox install + launch, AVCs.
5. Xwayland AHB path verified → remove linux-dmabuf, `dmabuf_import.c`, minigbm patch.
6. Presenter sync audit (from the earlier handover).

## Lessons
- luna sometimes "fixes" perf by adding I/O (spool) or leaves work uncommitted / on the wrong branch: review
  every diff, check `git status` in worktrees after each run.
- luna-low once claimed it had no shell; add "You DO have a shell tool" to read-only briefs.
- Briefs must say whether the agent boots its own VM; "you own VM 5556" alone made one skip the VM.
- Host tests used `bare-user` repos; devices use `bare-user-only` — test both modes.
