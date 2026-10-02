## Software Center repair, 2026-10-02

Current handover: [Software Center and Flatpak repair](HANDOVER-2026-10-02.md).
r11 (20261002, `daaf6fd6…`) is built with the deployment app-info icon
fallback (Brave's placeholder icon) and the newest broker/wrapper. r12
(20261002, `2e7251c5…`) is built with server-side-only window decorations
(double title bar fix) and the Xwayland plumbing (gated off until
`tools/stage-xwayland.sh` stages the binaries for r13). Both are awaiting
fresh-boot verification. Older r1-r10 images remain preserved. No build is
currently running. No release or push.

# Session handoff

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Paths: scripts derive the root; per-machine settings in matonos.local.env (git-ignored).
Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: device/maton/pc_x86_64 is the git working copy of github.com/MatonOS-dev/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
