# Handover 2026-10-23 night

Deadline (user): **Sunday 2026-10-25 evening** — Flatpak core + i386 (memory `deadline-2026-10-25`).
Work stopped at 23:00 (user cutoff).

## State at stop
- **v45 image build** (multilib: x86 libs, 64-bit-only zygote; approved Soong Rust patches) was started unattended:
  log `out/pc-logs/v45-build.log`. Check it finished; the final ISA audit step may still FAIL (see ISA below) — the
  images are written before that step.
- **VM is OFF** (stopped for the image build). Old disk kept: `~/matonos/vm/disk-vm/disk-v43.img`; current disk
  `disk.img` is from v44 with Platform 26.08 runtime + Tux Racer (uid 10129) installed.
- Agents: see "Open" for anything still running at 23:00 (check `out/pc-logs/agents/*.final.md`).

## Done today
- linux-subsystem-app **main 3a34913 pushed** (= egl-window): GLX 1.4/ARB_create_context/swap control, sweep fixes
  sys F1 (native EGL handles + locked refcounted side table), F3 (stopped windows get frame-done), F6 (Vulkan
  acquire deadline), gpu F10 (EGL extension passthrough + probe blocklist), F11 (DNS: proxy rewrites :53 to a
  raw-clone helper speaking netd's dnsproxyd protocol — no bionic; i386 too), NEWNET interface ioctls on any
  socket (fixed first launch of every fresh Flatpak), wrfsbase when AT_HWCAP2 FSGSBASE, x86-64-v2 enforcement +
  ISA checker in the subsystem build. Verified on v44 VM: core-profile, alpha.c 12/12, Tux Racer runs.
- v44 image: enforcing boot OK; disk VM regenerated.
- Device tree: dontaudit Mesa mesa.* probe (14e57a4); ISA audit + tools/check-isa.sh (8674ec9); kernel v2 pin
  restored (18005eb); Soong Rust sandybridge→corei7 patches 0001/0002 (approved in preflight/checks.py, d761f47);
  multilib BoardConfig (2d8b4bb); vendor Mesa built for i686 (1ed9c94, prebuilt/mesa/lib 210 MiB).
- Decisions → memory: dns-proxy-fake-reply, i386-gpu-venus (multilib libs + virtual TLS slots + modify_ldt
  emulation, 32/64-bit only), deadline-2026-10-25, support-policy (releases 1–2 strictly 6 months),
  codex-model-budget (luna → luna-high → sol-low after 2 luna-high fails, max 1 sol job / 5 h; log in
  out/pc-logs/agents/sol-jobs.txt — last sol job ~21:00).

## Open — resume here (in order)
1. **v45**: confirm build; copy to `~/matonos/vm/coord/v45.img`; regenerate disk VM (docs/DISK-VM.md; keep the v44
   disk as disk-v44.img); enforcing boot; check /system/lib + /vendor/lib (32-bit) present; rerun
   tools/check-isa.sh on the product — Rust crates must be clean now; then allowlist the dispatched codec/crypto
   libs (av1enc, libcrypto, libjxl, libvpx, swcodec apex, audiopreprocessing) per function; Mesa library-level
   allowlist (user: trust Mesa).
2. **Menu font fix** `lsa-gpuint` branch `menu-text` 08dbea0 (+902dd17 depth/stencil, atlas probes): sol found
   glBindFramebufferEXT/glBindRenderbufferEXT missing from the generated forwarder + GLES/desktop thunks sharing
   355 driver-pointer caches. Verify on VM: reinstall Tux Racer state if the disk was regenerated, run
   glx-shim3/run129.py with atlas2/atlas/alpha/core-profile/depth-stencil, then menu screenshot (ENTER) vs
   glx-shim3/host-reference-menu.png. Then merge menu-text → main, push.
3. **i386 TLS** `lsa-dns` branch `i386-tls` (8fa4024 virtual slots, 11947d5 modify_ldt table, 647f068 LDT selector
   remap on #GP, c6bca9a eager reservation + claim/release hooks; i386-tls5 = acceptance tests + run.sh exit-124
   timeout). Merge to main once the full proxy suite passes.
4. **i386 forwarders**: i386 builds of EGL/GLX/Vulkan forwarders + i386 thread-adoption backend using the reserved
   TLS slot (switch = %gs reload); GL.maton i386 extension; test a 32-bit GL app on v45.
5. Flatpak core: app-only install pulls its runtime (Calculator gap), smoke test 4–5 Flatpaks (GTK4, Qt, SDL,
   Electron) on a clean install.

## Notes / gotchas
- Builds rewrite tracked `linux/flatpak/prebuilt/static/SOURCE` — `git checkout --` it before switching branches.
- Agent output dirs need `deps/`, `broker/` and `flatpak/build/prefix` copied from main-out.
- run.py hard-codes the stub uid; use glx-shim3/run129.py (uid 10129) or regenerate for a new install; adb push
  into a never-launched app's home leaves it owned by 1000 (chown it).
- Host has Xvfb + `flatpak --user` Tux Racer for reference renders (python-xlib XTEST for keys, xwd → PIL decode).
