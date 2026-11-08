# Handover 2026-11-07 (evening): Webbed under GE-Proton Wayland

Goal: Webbed (Steam AppId 1390350, 32-bit PE) launching on the `-g native` VM
(adb localhost:5556) via GE-Proton11-12 + winewayland.drv.

## State at stop

- Webbed process now starts (error 731 fixed) and runs into DXVK, which fails:
  `DxvkInstance::createInstance: Failed to create Vulkan instance`
  (x86 DXVK, extensions VK_KHR_surface + VK_KHR_win32_surface), then exits.
  The i386 forwarder only logged `[i386] maton-vulkan: bridge libvulkan 0 0`,
  never `resident create` (the 64-bit side does). So the i386 Vulkan loader
  either did not find our ICD or failed before calling into it.
- Proxy tracing is OFF (cache/v3-trace removed). The logcat buffer on the VM
  is set to 64M (`logcat -G 64M`, not persistent across reboot).
- Nothing committed. Repo ~/matonos/repos/lsa-trim, branch `xwayland-ahb`,
  large uncommitted tree (also older WIP from 11-06: vulkan-forward wsi/icd,
  ahb_buffer, bionic32, net). Stray *.o files in linux/compat-proxy/ are
  build leftovers, do not commit them.
- Latest APK built and installed on 5556 (contains everything below).

## Next step

Launch options to set in Steam (Webbed > Properties > Launch options):

    PROTONFIXES_DISABLE=1 PROTON_ENABLE_WAYLAND=1 PROTON_LOG=1 VK_LOADER_DEBUG=error,warn,driver WINEDEBUG=+vulkan %command%

(currently still `... WINEDEBUG=+service,+rpc,+ntdll,+file,+process`, which is heavy.)
Then read steam-1390350.log for which i386 ICD manifests the loader saw and
why vkCreateInstance failed. Compare with the stock-Proton X11 i386 path,
which did reach the forwarder (black-screen run earlier today).

Logs:
- Proton: /data/data/flatpak.app.com_valvesoftware_Steam.x86_64.stable/home/.var/app/com.valvesoftware.Steam/steam-1390350.log
- Steam console: .../.local/share/Steam/logs/console-linux.txt
- Proxy trace: `touch <stub>/cache/v3-trace` + chcon `u:object_r:app_data_file:s0:c173,c256,c512,c768`,
  then stream `adb logcat -s MatonV3` to a file (writes about 3 GB per launch; turn it off afterwards).

## Changes today (uncommitted)

1. **loader.c, fixed-address ELF staging (fixes 731).** The i386 `wine-preloader` is
   ET_EXEC with PT_LOADs at 0x08048000 and 0x7d400000. map_elf staged the whole
   1.9 GB hull, so `resident_collision` saw it overlap the resident proxy and
   returned ENOEXEC. Had that check passed, commit_elf would have mremapped gap
   pages over live mappings. Now `struct mapped` holds `spans`
   (ET_DYN = 1 span; ET_EXEC = one span per run of touching PT_LOADs), with a
   collision check, staging and commit per span, and `release_spans()` on
   failure/commit. Verified on device: `exec-map-main 0 2`.
   Host: static ET_EXEC loads. The suite stops at `tests/marker-cache.py`
   (link() inside a `.v3-store-root` dir returns failure). The same happens with
   the old staging code, so it is a pre-existing bug and still open.
2. **proxy.c, sysctl virtual files:** `/proc/sys/fs/file-max` served as
   9223372036854775807 (plus overflowuid/gid 65534) from a memfd. Protonfixes
   needed it.
3. **init.pc_x86_64.rc** (device tree): `on init` writes file-max = LONG_MAX.
   Needs an image rebuild (stop the VM first). Must stay in sync with proxy.c.
4. **compositor_core.c, modifiers:** key commands carry Android metaState.
   `sync_android_meta()` resyncs xkb depressed/locked mods (skips the modifier
   keys themselves) and logs a warning on divergence. The focus-lost release
   message now uses the Android log. This came from the stuck Caps Lock/Shift
   bug: Caps Lock was latched on the VM's AT keyboard (clear it with sendevent
   on /dev/input/event1, not `input keyevent`).
5. Still in tree: TEMPDIAGs (vulkan-forward backend.h `%fs` check,
   wayland_resolve.c dlopen diag), plus `proxy_exec_mprotect` and the
   wayland_resolve PT_PHDR guard. Remove the TEMPDIAGs before commit.

## Setup notes

- GE-Proton11-12 is installed in Steam/compatibilitytools.d (owner u0_a173;
  labels set via chcon, including `chcon -h` for the 1953 symlinks). Tarball:
  session scratchpad ge/ge.tar.gz (gone after the session; re-download if
  needed).
- GE wine uses the old 32-bit wow layout (lib/wine/i386-unix/wine-preloader),
  not new WoW64.
- winewayland connects to our compositor. It warns about missing optional
  protocols: text-input-v3, ext_data_control, xdg_toplevel_icon,
  fractional-scale, xdg_activation.
- services.exe exits with code 1 in untraced runs (RPC 1722). Not fatal so far.

## Pending reports

The codex reports vkcts-build-1 and ahb-modifier-1 (`.last.md` in
out/pc-logs/agents) are still to be relayed to the user.
