## Software Center repair, 2026-10-02

Current handover: [Software Center and Flatpak repair](HANDOVER-2026-10-02.md).
r14 (20261002, `8d167425…`) is the current build and the one to test the
MC launcher on: the Xwayland baked-path fix (r13's Xwayland never worked
on device), wlroots logs in logcat (MatonWLR), bridge launch-failure
logging, plus all r13 content (xcb-closure APK fix, real Xwayland staged,
offer-both display selection, readable stub names `flatpak.<id>`).
App icons confirmed working by the user on r12. r11-r13 remain preserved.
No build is currently running. No release or push.

# Session handoff

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Paths: scripts derive the root; per-machine settings in matonos.local.env (git-ignored).
Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: device/maton/pc_x86_64 is the git working copy of github.com/MatonOS-dev/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
