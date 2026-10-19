# Handover 2026-10-19 (night)

Read after `HANDOVER-2026-10-18-night.md`. Agent briefs/logs/reports: `out/pc-logs/agents/<name>.{md,log,last.md}`.

## Headline
- **Vulkan works through the whole chain:** VulkanInfo (Flatpak, launched from its Android stub, enforcing) lists
  `Virtio-GPU Venus (AMD Radeon RX 6600 (RADV NAVI23))` via GL.maton + freestanding forwarder + resident bionic bridge,
  exit 0. Repro needs the venus VM (below).
- **vkcube** gets a window and a GPU device but no frames yet: blocked by the VM's gralloc (fix in the image now building).
- **Proxy speed:** host warm pull+deploy 26.9 s -> 4.83 s (native 3.2 s); VM VulkanInfo install 2h20 -> ~22 min.
- **Image v41 is building** (`out/pc-logs/test-build.log`, status `out/pc-logs/agents/build-status.txt`); v40 bootlooped
  on privapp enforcement (fixed, see below).

## Decisions today (user)
- **Not a security sandbox** (now in AGENTS.md + agents/common.md): the Linux layer fills gaps in Android; Android's app
  sandbox is the boundary. The whole v2 security section of AGENTS.md is marked superseded (named exceptions still valid).
- **Binder guard removed entirely** (option 2): Linux processes may use Binder from any code.
- **Site rewriting** (design in subsystem `linux/compat-proxy/SITE-REWRITE.md`): x86_64 = `mov $stub,%eax; jmp *%rax`
  over `mov $nr,%eax; syscall` (stubs < 4 GiB; register-number sites use `jmp rel32`); arm64 = plain `svc`->`b stub`;
  riscv64 = no rewriting for now. **Implement only after the cheaper proxy fixes** (per-trap openat/readlinkat probing,
  anon mmap passthrough D1, install filter without path traps).
- **Next image when Flatpak works**; the venus gralloc fix counts as making Flatpak work (so v40/v41 are allowed).
- **If Vulkan doesn't work, get OpenGL working** (EGL window surfaces are implemented, branch `egl-window`, VM test pending).
- **Codex budget:** sol-low use for Vulkan exhausted codex at ~17:46 (reset 18:07). Default back to luna-high; sol only
  if luna-high fails on a well-specified task.
- Approved + done: remove made-up limits (PipeWire JOBS<=4, vfs_state 65536 caps), free trap arenas on thread exit.
- Disk-backed test VM via `tools/installer.sh` approved, **not started** (RAM /data causes lowmemorykiller on big installs).
- Future: virtual terminal for terminal-only Flatpaks (stdout/stderr now only in logcat `MatonV3`).

## State
- **Subsystem main = `2324201`** (pushed). Today: ldconfig/packed bind-fd/getdents fixes, GL.maton as real installed
  extension, linker64 exemption, SIGSYS/altstack fixes, fcntl F_GETFD/F_SETFD passthrough, per-thread trap arena,
  staging peak fix (+ my bare-user pack-repo restore cc04bd3), audio asset fix, v3-vkdebug + v3-command debug switches,
  Binder guard removal, client-owned AHB swapchain import, ahb-native probe.
- **Device tree main = `eb78a69`** (pushed): `MATON_QEMU` in run-qemu-live, venus-gralloc docs + manifest pin minigbm
  `matonos/v26.10`, gpuservice sepolicy, AGENTS.md principle, privapp allowlist for the System Bridge.
- **minigbm fork:** new branch `matonos/v26.10` (`c56630c`, pushed to MatonOS-dev/android_external_minigbm):
  drv.c selects minigbm's virtgpu backend for DRM driver `virtio_gpu` (GBM Mesa fallback), every other GPU unchanged.
  `matonos/v1.2` stays for old builds.
- **Unmerged:** subsystem branch `egl-window` (`8557d20`, worktree `~/matonos/repos/lsa-gpuint`): EGL Wayland window
  surfaces (pbuffer -> copy into client AHB -> maton_ahb_buffer_v1). Host checks pass; merge after a VM test.
- **Unmerged:** subsystem branch `host-os-release` (`f0e3d1b`): generated MatonOS /etc/os-release in the host view +
  logo at /run/host/matonos/logo.txt; host tests pass, VM test pending (check `cat /run/host/os-release` in a Flatpak).
- Worktrees: `lsa-vkwsi` (free), `lsa-gpuint` (egl-window), `lsa-vkcube` (merged; remove with `git worktree remove`),
  device tree `out/worktrees/venus-gralloc` (merged; remove).
- **No VM running.** /data was RAM; everything installed is gone.

## Venus VM (needed for Vulkan on QEMU)
System QEMU 9.0.2 is too old. Local build in `~/matonos/qemu-venus` (QEMU 10.1.0 + virglrenderer 1.1.1 `-Dvenus=true`):
`MATON_QEMU=~/matonos/qemu-venus/bin/qemu-system-x86_64 tools/run-qemu-live.sh -i <img> -g venus -m 12288 -a 5556 -s ~/matonos/vm/coord/serial.log`
Copy images to `~/matonos/vm/coord/v<N>.img` first (never boot from /mnt/data). Host RAM is 23 GB: Soong analysis
takes ~19 GB, so never run analysis and a 12 GiB VM together.

## Next (in order)
1. **v41 image** (building, started 21:19, ETA ~22:10): copy to `~/matonos/vm/coord/v41.img`, boot venus, normal
   (enforcing) entry. Check it boots (privapp), `sys.boot_completed`, then:
2. `adb install -r` the current subsystem APK (build from main: `MATON_BUILD_JOBS=8
   MATON_SUBSYSTEM_OUT=~/matonos/build/main-out` + signing env, `./build.sh`), install VulkanInfo via
   `~/matonos/tools/VulkanInfoTestInstaller.apk` (`am start -n org.matonos.testinstaller/org.matonos.linux.VulkanInfoInstallActivity`).
   Time the install (speed check).
3. **vkcube:** `<stub cache>/v3-command` = `vkcube`, `v3-vkdebug` touch (chown to stub uid + restorecon), force-stop,
   `monkey -p <stub> 1`, screenshots. First run the native probe
   (`linux/vulkan-forward/tests/ahb-native-probe.c`, NDK API 36) — with the virtgpu gralloc it must no longer abort in
   `vkGetAndroidHardwareBufferPropertiesANDROID`. If the gralloc change breaks SurfaceFlinger/composer, check
   allocator/composer logs first.
4. **GL:** install glmark2 (`org.glmark2.glmark2`, has es2-wayland) and test the `egl-window` branch; merge if it works.
   If Vulkan still fails, GL is the priority (user).
5. Cheaper proxy fixes (then site rewriting): per-trap openat/readlinkat probe attribution (~46 raw calls/trap left),
   anon mmap passthrough, install-process filter without path traps.
6. Disk-backed VM via `tools/installer.sh`; staging still holds several packages at once on big installs.
7. Re-pin the bootstrap APK to current subsystem main (image still ships the 2026-10-18 bootstrap pin).

## Lessons
- **privapp enforce bootloop:** system_server throws `Signature|privileged permissions not in privileged permission
  allowlist` and loops; adb stays `offline`. Read logcat via `tools/qemu-shell.py <serial.log.sock.PORT.PID> '<cmd>'`
  (kernel audit lines interleave; write to a file on the device, then cat). Permissive mode does not help (not SELinux).
- **The 99% of traps were upstream glib's per-spawn CLOEXEC walk** (1M fcntl, native flatpak does it too): don't chase
  "broken fast paths" without comparing native strace first. Same for ICD duplicates (desktop flatpak lists 2x too).
- **strace -c on the VM** separates payload traps (`rt_sigreturn` count) from the proxy's own raw calls; found the
  mmap-per-allocation allocator (~390 kernel calls per trap).
- **Venus AHB import** needs blob/shareable virtio resources; minigbm `gbm_mesa` makes plain virgl GL resources
  (host `vkr: invalid res_id`). Symbolize guest Mesa with `out/pc-mesa/android/src/virtio/vulkan/libvulkan_virtio.so`;
  host render-server errors are in `journalctl --user`.
- **Briefs:** evidence files as absolute paths; never name evidence `<job>.log` (run-task.sh writes it); rebasing agent
  branches can conflict (pack_shadow vs vfs_palloc) — rerun the full proxy suite after.
- **Review agent fixes:** luna removed the separate bare-user pack-repo (exec bits became 0600), forced
  `VK_DRIVER_FILES` (rejected), freed arenas with a cross-thread use-after-free (fixed: only own arena is searched),
  added sources Soong already compiled (minigbm). Read every diff.
- **APK versionCode = commit count + 2:** a branch with fewer commits than what's installed is rejected as a downgrade.
- Codex usage limit: say the reset time, schedule the relaunch after it, never probe the limit.
