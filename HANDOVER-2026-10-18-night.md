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
