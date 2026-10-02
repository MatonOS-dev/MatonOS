## Software Center repair, 2026-10-02

Current handover: [Software Center and Flatpak repair](HANDOVER-2026-10-02.md).
r13 (20261002, `1141086508…`) is the current build: the compositor APK
crash fix (xcb closure bundled — r12's compositor died at load and no app
could launch), the real Xwayland server staged into system_ext,
offer-both display selection, and readable stub names `flatpak.<id>`.
App icons confirmed working by the user on r12. r11/r12 remain preserved.
Awaiting fresh-boot verification of r13 (checklist in the handover). No
build is currently running. No release or push.

# Session handoff

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Paths: scripts derive the root; per-machine settings in matonos.local.env (git-ignored).
Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: device/maton/pc_x86_64 is the git working copy of github.com/MatonOS-dev/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
