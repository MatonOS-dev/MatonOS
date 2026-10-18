# Handover 2026-10-18 (night)

Read after `HANDOVER-2026-10-17-night.md`. Agent briefs/logs/reports: `out/pc-logs/agents/<name>.{md,log,last.md}`.

## Decisions today (user)
- **Subsystem = untrusted app:** `org.matonos.linux` runs as `untrusted_app_34` (seapp_contexts, isPrivApp=true) with
  only INSTALL_PACKAGES, DELETE_PACKAGES, START_ACTIVITIES_FROM_BACKGROUND privileged. Never back to priv_app.
- **Bootstrap APK:** the image ships only `MatonLinuxSubsystemBootstrap.apk` (versionCode 1, StubLoader + boot receiver,
  no native code) embedding the real APK; it installs it over itself on first boot / after "uninstall updates".
  Later updates only via our F-Droid repo on download.hanro50.net.za/matonos (work on it after Flatpaks work).
  Image/policy changes must stay compatible with the previously shipped subsystem.
- **Running stubs keep running** during a subsystem update (new host on next launch). No kill-on-update.
- **StubLoader (option A):** never trust `sharedLibraryFiles` (stale after update-over-update); resolve the installed
  host via PackageManager at the first component class lookup (static HostClassLoader).
- **Seccomp:** resident bionic libc's text is exempt from the proxy filter by IP range (after the Binder guard).
  **Our own Syscall User Dispatch use is removed**; the proxy still emulates an app's own SUD (Wine).
- **D-Bus:** apps get the stub's private broker bus directly (`--socket=session-bus`), no xdg-dbus-proxy.
- **GL extension:** forwarders become Flatpak GL extension `org.freedesktop.Platform.GL.maton`
  (`FLATPAK_GL_DRIVERS=maton:default`), replacing the `/run/maton` launch maps (brief ready: `gl-maton-extension.md`).
- **Host os-release:** generated MatonOS `/etc/os-release` from Android version props (`ID=matonos`, `VERSION_ID` =
  ro.build.version.release, `LOGO=matonos`); the ASCII logo (`linux/subsystem/host-files/matonos-logo.txt`) is a
  system-provided file visible in sandboxes under /run/host (brief ready: `host-os-release.md`). No upstream logo PR.
- **privapp enforcement on** (`ro.control_privapp_permissions=enforce`); microG allowlist completed (deny DUMP, MANAGE_USB).
- **QEMU:** fixed window at `-r` size by default, `-F` full-screen (Launcher3 taskbar crash came from resizing).
- sol-6.1-low allowed for the VFS/bwrap chain (user, tonight). Disk cleanup of finished agents done (/home 85%).

## State
- **Subsystem main = `82651f7`** (pushed). Today: bootstrap + versionCode = commit count (`2a896ce`, `61dee8d`),
  StubLoader via PackageManager (`1758108`), FICLONE -> EOPNOTSUPP (`9a1dfb4`), bionic seccomp exemption + SUD removal
  (`6a9cf82`, incl. 4 GiB range split), session bus direct (`92d87b1`), process clones via proxy (`c580743`),
  packed lookups/root/identity/EEXIST/oldroot/exec/O_PATH (`2704b5f`..`82651f7`), logo file (`0cbecdb`).
- **Device tree main = `1780cae`** (pushed): untrusted domain (`8169664`), bootstrap pin + no image .so copies
  (`6e09737`, `004aa91` = pin 1758108, embedded real versionCode 399), privapp files installed as `.xml`
  (`4253686` — before this NO privapp allowlist was read), initial-package-state not stopped (`d22ce84`),
  QEMU window (`a2db779`), privapp enforce + microG (`1780cae`, **not in any image yet**).
- **Latest image:** v39 (`~/matonos/vm/coord/v39.img`, built 15:04 from pin 1758108), booted on VM 5556
  (`-m 12288`), enforcing, VulkanInfo installed, proxy trace on in the stub's cache. VM still running; it has the
  newest subsystem APK adb-installed (`82651f7`-era). Do not reboot it (/data is RAM).
- **lsa-vkwsi worktree:** on branch `ldconfig` (luna's 9 mkdir-workaround commits — NOT merged, reference only;
  reverted direction). Stash holds the superseded spawner attempt.

## VulkanInfo launch chain on main (where it stops)
stub -> StubLoader -> loader -> flatpak (session bus direct) -> bwrap (clone, pivot, usr-merge links, packs) ->
**flatpak's ldconfig sandbox fails: `ldconfig failed, exit status 256`** -> app not started.
Earlier in the day, with a VFS alias (since removed), vulkaninfo itself ran and failed `ERROR_INCOMPATIBLE_DRIVER`.

## Blocker -> first thing tomorrow
1. **ldconfig:** brief `out/pc-logs/agents/ldconfig-sol.md` is written but the sol run FAILED TO START: model id
   `sol-6.1` is wrong ("not supported when using Codex with a ChatGPT account"). Find the correct sol model id
   (memory codex-model-budget / earlier agent logs) and launch with `MATON_CODEX_MODEL=<id> MATON_CODEX_EFFORT=low
   bash run-task.sh ldconfig-sol`. Hypothesis: the ldconfig bwrap reaches the packed runtime / GL extension mountpoint
   by a VFS route the main-bwrap fixes don't cover. First capture ldconfig's real stderr.
2. Then `ERROR_INCOMPATIBLE_DRIVER`: one run with `VK_LOADER_DEBUG=all` decides (manifest not found / ICD negotiation /
   Android side). `gl-maton-extension` replaces the manifest path.
3. Queue: `host-os-release`, `gl-maton-extension` (briefs ready).
4. Next image (includes privapp enforce): first check it boots (enforce bootloops on any missed allowlist entry),
   the bootstrap self-installs without manual start, and the "uninstall updates" fallback shows the preparing screen.

## Lessons
- luna fixes symptoms in the VFS/bwrap area (fake mkdir success, synthetic EEXIST, /lib64 alias, unenforced openat2
  flags) and stops after 2-3 minutes at a hypothesis. Read bwrap/flatpak source + traces yourself first; brief one
  cause per task with "instrument first"; review every diff; rerun suites yourself (it skips MATON_AOSP_ROOT).
- strace on the VM: match exact comm (`pgrep -x libmatonos-v3-l`), never `pgrep -f` (self-match).
- `adb shell` inside a `while read` loop eats stdin: use `</dev/null`.
- coord-build.sh kills test VMs; image builds stop the agent VM.
- Live-image VM reboot = factory reset (added to common.md).
- Hourly status cron (`f12875e6`) is session-only; it dies with this session.

## Remaining work (detailed, in order)

### A. Get VulkanInfo running (critical path)
1. **ldconfig sandbox** (`ldconfig-sol.md`, sol-low; fix the model id first).
   - Capture ldconfig's own output and the proxy trace of flatpak's second bwrap (flatpak-run.c regenerate_ld_cache).
   - Fix the VFS/mount route it takes (likely the GL extension mountpoint / packed runtime via a path the main-bwrap
     fixes don't cover). Kernel-exact; no skipping ldconfig, no fake cache, no mkdir placeholders.
   - Host regression; then on the VM vulkaninfo must start again.
2. **`ERROR_INCOMPATIBLE_DRIVER`.** Run once with `--env=VK_LOADER_DEBUG=all` and read the loader log:
   - manifest not found -> solved by GL.maton (step 3) or the /run/maton bind;
   - ICD negotiation/exports -> fix in linux/vulkan-forward (stays freestanding, no libc);
   - Android side (GPU bridge / resident bionic Vulkan) -> bridge fix.
   Done when `vulkaninfo --summary` lists the forwarder device in a Flatpak via the stub path.
3. **GL.maton extension** (`gl-maton-extension.md`): build the extension from the subsystem repo, ship in the APK,
   install like other runtime extensions, `FLATPAK_GL_DRIVERS=maton:default`; delete V3Launcher's /run/maton launch
   maps + VK_DRIVER_FILES/__EGL_VENDOR_LIBRARY_FILENAMES overrides. Verify vulkaninfo + EGL vendor found.
4. **host-os-release** (`host-os-release.md`): generated /etc/os-release in the host view (consistent stat/open,
   pre/post pivot) + logo at /run/host/matonos/logo.txt; README documents `fastfetch --logo`.

### B. Next image (one build, after A1-A2 land; subsystem pin first)
1. Pin subsystem main into the device tree (bootstrap APK + SOURCE hashes, embedded versionCode).
2. Contents already on device-tree main but untested: privapp enforce + microG allowlist (`1780cae`).
3. Boot checks, in order: boots at all (enforce bootloops on a missed allowlist entry -> read the system_server
   exception, allowlist or deny), bootstrap self-installs without manual start (initial-package-state), only one
   install session, `/data/app` copy runs as untrusted_app_34 with 3 privileged permissions, "uninstall updates"
   -> stub shows HostPreparingActivity and the bootstrap reinstalls.
4. Then reinstall VulkanInfo via `~/matonos/tools/VulkanInfoTestInstaller.apk` and repeat A on the fresh image.

### C. Graphics after Vulkan works (from 2026-10-17 plan)
1. EGL step 1 acceptance: pbuffer readback glibc+musl, `eglinfo` in a Flatpak; step 2 Wayland + X11 surfaces;
   step 3 Zink fallback (Mesa from GL.default behind GL.maton in glvnd order).
2. vkcube windowed: first VM test of the Vulkan Wayland WSI + AHB wl_buffers (`0fbaca3`, host-tested only).
3. Xwayland on Android EGL + AHBs over maton_ahb_buffer_v1 (agent xwayland-android-gles-2 never implemented it).
4. After Xwayland's AHB path is verified and no client sends raw dma-bufs: remove linux-dmabuf, dmabuf_import.c,
   the minigbm patch.
5. Presenter sync audit (from the 2026-10-17 handover).
6. Firefox install + launch, AVC sweep.

### D. Proxy / platform follow-ups
1. Seccomp filter: allow `mmap` with MAP_ANONYMOUS natively (only file-backed mmap needs the proxy). Separate commit
   + test; perf win for malloc-heavy apps.
2. Regression test for a Wine-style payload that sets up its own SUD in GPU-bridge mode (now that our SUD is gone).
3. Install speed: VulkanInfo took ~25 min (download phases slow, user: "shouldn't take that long"). Profile
   download vs pack vs Android install; compare with host `flatpak install` speed on the same network.
4. Remaining denials during install/launch (low): search on shell_test_data_file (/data/local/tmp), getattr on
   linkerconfig dir, TCGETS on pipes; allocator HAL reading default_prop (~130x at boot) — find the prop, label it.
5. The 2 stale agent branches in lsa-vkwsi (`ldconfig`, stash `sigsys-159c`) — delete once A1 lands.

### E. Later (user-ordered)
1. F-Droid repo for the subsystem on download.hanro50.net.za/matonos — after Flatpak apps work end to end.
2. Software centre (own store, F-Droid + Flatpak sources) per the existing plan.
3. Podman addon, Digitalis addon — unchanged, not started.
