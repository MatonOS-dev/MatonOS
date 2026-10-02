## Software Center repair, 2026-10-02

Current handover: [Software Center and Flatpak repair](HANDOVER-2026-10-02.md).
The r7 build was terminated; no r7 image exists. Latest completed live image
is r6, with confirmed runtime failures. No build is currently running.
Detailed chronology: [repair log](../../../out/pc-logs/agents/REPAIR-HANDOFF-2026-10-01.md).
No release or push.

# Session handoff

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Paths: scripts derive the root; per-machine settings in matonos.local.env (git-ignored).
Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: device/maton/pc_x86_64 is the git working copy of github.com/MatonOS-dev/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
