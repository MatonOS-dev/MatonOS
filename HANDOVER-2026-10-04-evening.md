# Handover — 2026-10-04 evening (r24: Flatpak storage redesign)

Read with `HANDOVER-2026-10-04-afternoon.md`. The design of record is
`install/linuxd/CODE-STORAGE-r24.md`; this file is the state and the queue.
Nothing below is device-tested yet; all waits for the next image build.

## Design decided today (details in CODE-STORAGE-r24.md)
- **Installer = upstream Flatpak outside bionic.** A separate APEX
  `com.matonos.flatpak.glibc` (glibc rootfs + static NDK entry helper) proved
  install/update/uninstall as root on a patched test image. The user wants the
  non-bionic side as small as possible, ideally **fully static (musl)**
  binaries with no rootfs at all — spike pending (below).
- **Storage = stock Flatpak installations, hardlinked.** Each stub uid owns a
  Flatpak USER installation at `/data/matonos/linux/apps/<uid>/`
  (`app/<id>/<arch>/<branch>/<commit>` + `active`, and `home/` for writable
  data). ONE preinstalled app **"MatonOS Linux Runtimes"**
  (`org.matonos.linuxruntimes`) owns the Flatpak SYSTEM installation with all
  runtimes. Code is hardlinked from a shared OSTree repo via
  `ostree pull-local` + stock `flatpak install --no-pull`; `flatpak run` works
  with both installations read-only (layout spike, proven on the host).
  Flatpak deployments are published from the shared staging repository and
  writable app state stays in each app's home directory.
- **Trust:** app stubs carry the signed OSTree commit (+ runtime ref/commit)
  in their manifest. linuxd uses the stage-only AID 2902 helper for
  signature-verified staging, checks commit pins, then uses `pull-local` and
  `install --no-pull` to publish.
- **Flatpaks may run code they download into their own data** (deliberate
  exception to "containment beats compatibility"). Needs the one AOSP patch
  `patches/system/sepolicy/0001-data-exec-exempt-domain.patch` (attribute
  `data_exec_exempt_domain` on `matonos_linux_app`), approved in preflight.
  The appdomain route is impossible (zygote-only entry, no capabilities); no
  binder exception — Flatpaks never get binder.
- Sandbox domain renamed **`matonos_linux_app`**; data label
  `matonos_linux_data_file`.
- Session sockets stay compositor-owned (host audit). Portal intents and the
  Inhibit wake lock now come from the stub (stubs request WAKE_LOCK).
- Rejected/dropped: W+X permission + second domain, `seapp_contexts` stub data
  label (untrusted apps can't create other labels), per-app dirs inside app
  data, a Linux VM for Steam (GPU), the `matonos_linux` stub group (gid
  mappings need a preinstalled declarer; stubs use per-device keys) — linuxd
  relies on the bridge's `ownsStub`.
- Installer identity: **AID 2902 `AID_VENDOR_MATONOS_FPINSTALL` (renamed: AID names must be < 32 chars)**
  (OEM system uid: exempt from netd's per-uid firewall that blocked uid
  29000). Network rights only in its own domain `matonos_flatpak_installer`.
- Glue that runs inside the non-bionic namespace (matonos-bwrap,
  matonos-app-exec) gets **statically linked** with the NDK; audit for system
  properties, bionic DNS, dlopen, liblog first.
- Steam = acceptance test (native game + Proton) once launch runs through the
  new stack.

## Merged on main today (not pushed)
0883847…82047a6: storage doc rewrites; linux-data (rename, `home/` with exec,
cleanup sweep, project-quota tagging); preflight approval of the sepolicy
patch; stub-intents; stub-commits + Linux Runtimes app (APK builds; bridge
API 8, bridge cert allowlist regenerated); AID 2902; helper-trim (mount
helper no longer mounts code; interim rule lets `matonos_linux_app` execute
the read-only Flatpak deployment). Earlier: helper-apk/apk-images (now
largely retired), mount-helper neverallow fix 26158dc.
`tools/preflight.sh` passes on main. **Policy rig caveat:** the rig compiles
against the platform CIL from the LAST image build (unpatched), so it fails
exactly on our execute rules (domain.te ~1982/~2051); only a real image
build validates them.

## Branches held (not merged)
- `ds/glibc-integrate` (687e6fd): glibc APEX in tree, static entry helper,
  installer domain net rights. Holds a **49 MB Debian rootfs** — merge only if
  the musl-static route fails. Still links malcontent (install --no-pull would
  want a system bus).
- `codex/glibc-flatpak`: original spike (patch-image.sh, test scripts).

## Next steps (in order)
1. **Static musl build — codex** (limit resets ~18:23): brief
   `out/pc-logs/agents/musl-static-116.md`. Flatpak **1.18.4** / bwrap
   **0.13.0** (pinned on main by e822e68 — my earlier "1.16.x" was stale),
   curl backend, no libsoup, never downgrade. Reuses the Alpine 3.21.3 build
   root in `out/pc-logs/musl-static/`; earlier static link failed on
   `-lffi` ordering. Manual command sequence was given to the user in chat.
2. Decide: static APEX (if 1 passes) vs merge `ds/glibc-integrate`.
3. Publish pipeline in linuxd (pull → fsck → relabel → pull-local →
   install --no-pull → active) + `matonos_linux_code_file` policy.
4. Image build (validates the sepolicy patch for real) + VM end-to-end:
   install and launch a Flathub app (Calculator was used in the spike).
5. Launch through the new stack (static glue, xdg-dbus-proxy stand-in on the
   broker / dbus-java), retire bionic Flatpak into a revivable `retired/`
   folder only after apps run, then Steam.
- Resolved 2026-10-05: removed the retired mount-helper source and policy,
  and removed `RUN_DOWNLOADED_CODE` declaration and stub permission logic.
  Flatpaks may execute downloaded code in their own data by default, covered
  by the existing `data_exec_exempt_domain` patch.

## Agent notes / lessons
- Codex hit its usage limit at 17:08; DeepSeek took over but is **low on
  credits** — no new DeepSeek work. Claude subagents burn tokens too fast
  (user stopped a Sonnet agent) — long jobs go to codex.
- Agents briefed with common.md edited the MAIN tree once (common.md names
  its absolute path): every brief now starts with a HARD RULE naming its
  worktree; never `git commit -a` on main while agents run.
- DeepSeek reviews needed real fixes every time (fabricated loop ABI, tab vs
  space record format, missing WAKE_LOCK, a domain-wide network grant) —
  always review before merge.
- The VM on adb port 5562 is not ours; never touch it.
