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

## Notes: what I learnt today

### Android packaging / PackageManager
- **SystemConfig only reads `*.xml`.** `prebuilt_etc` names the installed file after the module unless
  `filename_from_src: true`; our four privapp allowlists were installed without an extension and never read.
  Permissions only worked because `ro.control_privapp_permissions` was unset (log-only). Check
  `ls /system_ext/etc/permissions/` on every image that adds a config file.
- **System apps start "stopped"** on first boot (`scannedAsStoppedSystemApp=true`) and receive no BOOT_COMPLETED.
  Exempt with `<initial-package-state package=... stopped="false"/>` in a permissions XML.
- **An updated system app keeps only what the system version declared:** privileged permissions and `<library>`
  shared libraries. Hence the bootstrap declares the same permissions and `org.matonos.linuxhost` as the real app.
- **Update-over-update keeps the old shared-library entry** (`SharedLibrariesImpl.getAllowedSharedLibInfos`:
  "declares library ... that already exists; skipping"), so dependents' `usesLibraryFiles` point at a deleted APK.
  The platform still builds the dependent's library loader from the **system** copy first (log: "Configuring clns-N
  for other apk /system_ext/priv-app/..."), so StubLoader itself always loads; only the host lookup must not trust
  `sharedLibraryFiles`.
- **LoadedApk creates the AppComponentFactory twice** (before and after `instantiateClassLoader`) and passes its own
  loader to `instantiate*`. State in the factory must be static; load components from your own loader.
- **PackageInstaller:** `session.commit()` fails "Files still open" if the write stream is still open.
  `FLAG_UPDATED_SYSTEM_APP` is also set when the bootstrap itself is the installed update — it does not mean "the
  real host is installed".
- **Same versionCode over a system app** installs fine with `adb install -r`; for the bootstrap->real flow the real
  APK needs a higher versionCode (commit count + 2 in build.sh).
- **untrusted_app ioctl allowlist:** FICLONE (0x9409) on app_data_file is denied; libglnx tries it before every copy.
- Privileged permissions defined by the requesting app's own signer (Bluetooth, DownloadProvider, mainline modules)
  are signature grants and unaffected by allowlists — a naive "requested privileged perm not allowlisted" scan
  produces false positives for them.

### Kernel / seccomp / signals
- **SUD and seccomp are independent.** SUD's selector stepping aside does not exempt anything from a seccomp filter;
  seccomp still judges every syscall by instruction pointer.
- **A seccomp TRAP while SIGSYS is blocked (inside the SIGSYS handler) = kernel kills the process with SIGSYS**,
  no handler, no log — exit 159. Any libc (bionic) call inside the handler that issues a filtered syscall dies this
  way. pthread_create's stack mmap (128 KiB + guard = 180224 bytes, PROT_NONE, MAP_NORESERVE) is the signature.
- **BPF can only compare 32-bit words:** IP ranges must be split at 4 GiB boundaries; a piece ending exactly on the
  boundary must skip the upper check (its low word is 0).
- Only one SUD configuration exists per thread, so our own SUD use would collide with Wine's.
- **Kernel error order:** `mkdir`/create on an existing target returns EEXIST even on a read-only mount
  (fs/namei.c filename_create); EROFS only if the target is missing. bwrap relies on it (ensure_node_at).
- `O_PATH|O_NOFOLLOW` on a symlink opens the link itself (fstat gives S_IFLNK). libglnx's `glnx_chaseat` tries
  `openat2` and falls back to `openat` on ENOSYS — leaving openat2 unimplemented is legitimate; implementing it with
  unenforced RESOLVE_* flags is not.

### bwrap / flatpak internals that bit us
- **`--bind-fd`:** after the mount bwrap reopens the destination and requires `st_dev`+`st_ino` equal to the fd
  ("Race condition binding dirfd"); emulated binds must preserve the source identity.
- **Bind sources are opened via the old root after pivot_root**; the emulated old root must show the full pre-pivot
  view, including proxy virtual mounts.
- **flatpak emits usr-merge links itself** (`--symlink usr/lib64 /lib64` ...) only after `glnx_chaseat` finds the
  dir in the runtime fd — never alias them in the VFS.
- **Without `--socket=session-bus` flatpak starts xdg-dbus-proxy**, whose setup lists the real `/` (denied for apps).
  Our broker is already per-app and policy-checked, so the direct bind is right.
- `/run/host/os-release` comes from the host `/etc/os-release` first, then `/usr/lib/os-release`.
- flatpak's ldconfig runs in a **second bwrap** with its own mounts — main-bwrap fixes do not automatically cover it.
- `--no-session-bus` removes the bus entirely; `FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY` is not "direct".

### Debugging technique that worked
- **strace on the VM** (userdebug has /system/bin/strace) attached from a polling loop on the exact comm:
  `for i in $(seq 5000); do p=$(pgrep -x libmatonos-v3-l|head -1); [ -n "$p" ] && exec strace -f -tt -ff -o /data/local/tmp/st -p $p; done`
  run as a host-side background `adb shell`. It found the SIGSYS cause in one run after a day of guessing.
- Proxy trace mode: touch `<stub data>/cache/v3-trace` (chown to the stub uid, restorecon). Trace ids are the
  internal `PROXY_SC_*` enum (starts at `unknown=-1`), not kernel numbers — map with the enum order.
- Read bwrap's and flatpak's source for the exact failing message (`~/matonos/build/main-out/flatpak/cache/src/
  bubblewrap/bubblewrap.c`, `~/matonos/repos/flatpak/common/`) before briefing: every fix today came from that.
- `adb shell` reads stdin: inside `while read` loops use `</dev/null`. `pm list permissions -f` lines start with
  `+ permission:`.

### Process
- Each luna round moved bwrap one step; the value came from coordinator analysis + narrow briefs. Expect luna to
  stop early or work around; "instrument first, change after" in the brief helps.
- Verify the remote model id before launching codex with a non-default model (sol run failed silently: model
  `sol-6.1` "not supported when using Codex with a ChatGPT account").
- Image builds take 45–60 min (full analysis after the host reboot); subsystem APK builds ~3 min at 16 jobs — iterate
  with `adb install -r` of the real APK, rebuild images only for policy/device-tree/StubLoader changes.
- StubLoader changes need an image (stubs load it from the bootstrap copy).
