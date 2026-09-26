# Session handoff

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: the AOSP root is the git working copy of github.com/Hanro50/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
