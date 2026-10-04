# Handover — 2026-10-04 afternoon (r24 in progress)

Read with `HANDOVER-2026-10-03-evening.md` (r23 results, r24 history, lessons).
Device repo `main` pushed to `MatonOS-dev/MatonOS`.

## State at hand-off
- Merged + pushed today: a67bbff bus-proxy, 25d16ad broker-exec, 8211a9f
  review fixes, a8bc215 D-Bus Java step 1, 7c96027 H1 enforcing, e822e68 deps
  (bwrap 0.13.0, Flatpak 1.18.4, xdg-dbus-proxy 0.1.9), 793fbf4 minSdk 36,
  4afd519 code storage + app ownership, fe3417b setns mount helper (ptrace
  grant removed by coordinator), 3fc99da small fixes (machine-id reasons,
  controller prompt once, bridge derives stub ref, test script as uid 1000).
- Nothing of today's work is device-tested; all waits for the r24 image.

## Running at hand-off
- **DeepSeek `ds/flatpak-apex`** (worktree out/worktrees/flatpak-apex, brief
  out/pc-logs/agents/ds-apex-brief.md, log ds-apex.log, report
  ds-apex-result.md). Packages the Flatpak stack as updatable APEX
  `com.matonos.flatpak`. Review: same SELinux exec types as today, no new
  allows, policy rig PASS, every /system_ext path caller updated, APEX.md
  matches NOTES "Flatpak APEX updates". Flatpak/ostree binaries probably need
  a rebuild with /apex prefixes (commands in its report).
- **Codex** (usage limit reset 13:10): next job H5 — controllers via
  InputManager → linuxd-owned per-session uinput Xbox 360 pad (design
  out/pc-logs/agents/opencode-h5-result.md). Not started yet.

## Decisions today
- Flatpak goes into an APEX (supersedes the app-layer launcher plan in
  out/pc-logs/agents/glm-applayer-design.md, kept for reference + its
  amendments A1–A5). `flatpak run` stays the sandbox assembler; linuxd stays
  in system_ext; app layer keeps decisions. Format-neutral launcher parked
  until AppImage/OCI is wanted. Reason: app domains cannot transition into
  non-app domains (stock app.te:872 neverallow, compile-tested).
- APEX updates: staged install + reboot, rollback on failed boot; live images
  can't keep staged updates (RAM /data).
- While codex is rate-limited, DeepSeek takes codex's queue.

## Next steps (in order)
1. Review + merge ds/flatpak-apex; rebuild Flatpak binaries if required.
2. H5 with codex.
3. r24 image: linux/compositor/build-compositor.sh first, then build based on
   out/pc-logs/agents/build-repair-r23.sh with MATON_SELINUX_PERMISSIVE=1.
4. Test on headless virgl VM port 5562 (copy image to ~/matonos/vm/claude/;
   after boot `setprop persist.vendor.maton.sleep_idle_s 0` + `svc power
   stayon true`). List: APEX active (/apex/com.matonos.flatpak), Flatpak
   install/launch, Chromium bus + chrome://gpu, system-app broker, seccomp
   (Seccomp: 2), code storage + setns mounts, controller prompt once,
   force-stop kills Linux procs, targetSdk 36. Then
   sepolicy/r24-enforcing-test-plan.md enforcing.
5. Open: hourly check-ins until 18:00 (cron afbdc69e) — final summary at 18:00;
   UPDATE-PLAYBOOK.md question unanswered; later D-Bus Java step 2,
   PipeWire pass-through, Java portals, Digitalis.
