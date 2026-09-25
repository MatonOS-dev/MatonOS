# matonos-bootwatch prototype

Debug-entry-only read-only boot diagnostic prototype. It samples the bounded
last 600 logcat lines and `sys.boot_completed`, then emits a concise logcat
summary. It exits on completed boot. It does not restart services, gate boot,
write files, or publish properties. It recognizes the documented audio
`IModule/default` lazy-service failure and audioserver restart evidence.

The init rc is proposal material; normal boots do not satisfy its debug
property trigger. No shared image inputs are changed. See `INTEGRATION.md`
for the exact proposed bundle, policy, rc, and loader edits.

Run `./build.sh` to compile the x86_64/API 35 NDK binary into `out/bin/`.
Host logic test: compile `logic.cpp` and `tests.cpp` with a host C++17
compiler, then pass `captured-audio-hang.log`. The fixture captures the
failure sequence recorded in `out/pc-logs/agents/audio-bootfix.md` and
`COORDINATOR-HANDOFF.old-2026-09-25.md`.

Open work before any image integration: independently validate logd access
under enforcing SELinux; add strict boot deadline/milestones; optionally
persist a bounded report and properties with reviewed labels/rules; validate
on a fresh debug-entry image. No image or VM testing was authorized here.
