# Handover 2026-10-24 morning (overnight run, stopped 08:00)

Deadline: **Sunday 2026-10-25 evening** (memory `deadline-2026-10-25`). Work order (user, 00:30, memory
`priority-order-2026-10-24`): i386 done → 32-bit segmented memory emulation → proxy optimisation/simplification/
cleanup → multi-core installs → bug fixes.

## Running at hand-over
- `mountd-spike` (luna-high, owns the VM): stage 1 measures real bind mounts in a stub's mount namespace vs the
  proxy path (and erofs vs extracted dirs); stage 2 designs the mount daemon (`mountd/DESIGN.md`). Report
  `out/pc-logs/agents/mountd-spike.final.md`. Decision: memory `mount-daemon`.
- `proxy-fdbook` (luna-high, host only, lsa-gpuint): fd-bookkeeping rewrite to cut ~18 raw syscalls/trap. When it
  reports, send it back for the ADDENDUM in its brief (immutable pack-tree metadata index + native comparison
  benchmarks incl. 1 GB read).
- Last sol job started 06:36 (user granted the window early) → next sol window ~11:36.

## Pushed overnight
linux-subsystem-app `main` **91d85b1** (= egl-window):
- **i386 renders end to end on v46**: all four ELF32 probes PASS (EGL + GLX readback, GLX alpha, Vulkan enumerate);
  fix: discover bionic TLS without the seccomp-blocked get_thread_area; i386 GLX frame pacing (sol job).
- Tux Racer menu font fix (glBindFramebufferEXT/RenderbufferEXT thunks; GLES/desktop caches split) — verified on v45/v46.
- i386: TLS virtualisation, eager reserved GDT entry + claim/release, emulated modify_ldt, LDT selector remap,
  i386 adoption backend, i386 Vulkan/EGL/GLX forwarders, i386 bionic GPU bridge (device adoption test PASS),
  Flatpak i386 Compat + GL32.maton mounting (apps need `allow=multiarch`, as upstream).
- Segmented memory phase A: selector cache with eviction, lds/les/lss/mov/pop seg loads, base/limit/expand-down.
- Proxy: README condensed (design guide), dns-netd.h, trap microbenchmark, raw-syscall attribution, dynamic VFS
  resolution cache, fd lock without pid syscall, openat2 fast path (stat −13%; open unchanged — see below).
- Installs: parallel plan executor (bounded by CPU count), concurrent store operations with per-ref dedup, stale
  "Another install is active" gone; GNOME extension parents resolved ([ExtensionOf]); boot cleanup via JobService.
- Build: per-output Flatpak scratch (parallel builds safe), tracked SOURCE no longer rewritten by builds; `rg`→`grep`
  in forwarder checks; static matonos-flatpak prebuilt refreshed (v2-clean, symbols match).
Device tree `main` **ae0ea3b**: multilib (x86 libs, 64-bit zygote), Soong Rust corei7 patches (APPROVED_PATCHES),
i686 Mesa build script, 32-bit vendor Mesa labels + vendor libhardware anchor, ISA checker + full allowlist (v46 image
PASS), kernel v2 pin restored.

## Image / VM
- **v46** (`~/matonos/vm/coord/v46.img`, 3,286 MiB; +232 MiB vs v44 for the i386 userland): enforcing boot OK;
  disk VM = v46 (old: `disk-v45.img`). Subsystem, test installer, Tux Racer, GNOME Calculator, i386 Compat + GL32
  installed on it.

## Open — in order
1. **Real 32-bit app**: Steam Flatpak (multiarch) on v46 — also the test app for segmented-memory phase B.
2. **Segmented memory phase B** (user: do it, test with Steam): 32-bit code segments; design note in the
   compat-proxy README (no 16-bit). Start after mountd-spike frees the VM/worktree.
3. **Proxy speed**: translated open ≈ +109 µs, stat ≈ +53 µs over native; ~18 raw syscalls per trap. The cost is
   handler/fd bookkeeping, not the trap (~80 ns) or path walk. openat2 fast path STAYS (user: fast > simple). Mount
   daemon (memory `mount-daemon`) is the main speed path; proot only as a test-checklist reference (GPL, ptrace). Next: cut raw calls in fd bookkeeping.
4. **Installs**: VM checks not yet run — cancellation (test installer has no cancel control), fetch dedup
   (needs OSTree fetch counting), forced-failure isolation. Smoke test GTK4/Qt/SDL/Electron apps on a clean disk.
5. Later: image size trim (memory `image-size-trim`: 540 MB uncompressed .ko, erofs uncompressed); OTA server per
   `support-policy` (releases 1–2 strictly 6 months).

## Gotchas learned
- Chain pushes to green builds (`[ $r = 0 ] && git push`); one broken main was pushed and fixed overnight.
- After `./build.sh --build-dependencies` (JOBS ≤ 4), refresh the static prebuilt with
  `tools/refresh-flatpak-prebuilt-source.sh` and commit it — do not revert it (ISA symbols must match build IDs).
- Device-tree preflight forbids naming AOSP libraries in PRODUCT_PACKAGES — use a local vendor anchor module.
- Agents sometimes force-add `prebuilt/` binaries: check `git diff --stat @{u}..HEAD` before pushing the public repo.
- Codex usage limit hit once (~05:35, reset 05:41); continuation briefs resume from the worktree diff.

## Update 11:30 (before context clear)
- Subsystem `main` **9101375** pushed (flat i386 LDT code segments = segmented memory phase B code side; proxy fd
  index: open ~13.6 µs vs 2.75 native, stat ~8 µs). Branches: `steam2` 7c5bb53 (per-sandbox /dev/shm WIP, untested),
  `mountd-spike` 7397d23. Device `main` 200b534 (+ mountd/DESIGN.md); `wip/mountd-dev` d5903b0 = unvalidated snapshot.
- Running: **mountd-dev** (PID 2601400, device tree; daemon builds, now selinux_policy check → report
  `out/pc-logs/agents/mountd-dev.final.md`). On exit, `relaunch-steam2.sh` auto-starts **steam2** (boots VM itself;
  goal: Steam login window renders).
- VM is OFF. Never run Soong while the VM runs (memory `no-soong-with-vm`).
- Next: review mountd-dev → merge to device main + push → brief subsystem side (request mounts at stub launch,
  untrapped file syscalls when mounted, proxy fallback) → v47 image (VM off) → VM test. Then smoke tests
  (GTK4 Text Editor, Kate, SuperTuxKart, Electron, Steam).
- Sol window reopens ~11:36 (log `out/pc-logs/agents/sol-jobs.txt`).

## Plan (agreed with user, 2026-10-23/24)
Deadline Sunday 2026-10-25 evening: Flatpak core + i386. Work order: i386 done → 32-bit segmented memory → proxy
optimise/simplify/cleanup → parallel installs → bug fixes.

1. **Mount daemon (main speed path)** — user: "current path stays as a fallback, but a priv daemon gets added".
   - `matonos-mountd` init daemon, own SELinux domain in the daemon zone. Subsystem asks it over binder at stub
     launch; it verifies caller + stub PID (installed Linux stub, verified pack/dir sources), `setns` into
     `/proc/<pid>/ns/mnt`, bind-mounts /app, /usr, home. Mount points added to the root image.
   - No zygote/framework patch, no user namespaces, no seccomp change; apps still cannot mount; subsystem stays
     untrusted_app.
   - When mounts are present: file syscalls are NOT trapped (native speed). Proxy VFS translation remains the fallback
     (daemon absent, nested bwrap views, flatpak-spawn --host, paths outside the fixed view).
   - Open: packs live in APK splits → erofs image per pack (verified hash, loop mount) vs extracted dirs (spike data
     in `mountd/DESIGN.md`).
2. **Segmented memory**: 32-bit + 64-bit only, NO 16-bit. Code segments emulated (no bionic modify_ldt patch);
   modify_ldt emulated; flat code segments done (9101375). Phase B is validated with Steam.
3. **Steam**: install works (Compat.i386 planner fix); blocked at /dev/shm → steam2 branch (private /dev/shm per
   sandbox). Then CEF/login-window blockers. Apps need `allow=multiarch` (upstream model, no per-app hacks).
4. **Proxy**: keep openat2 RESOLVE_IN_ROOT fast path (fast > simple). fd-bookkeeping rewrite done; addendum = immutable
   pack-tree metadata index + native comparison benchmarks (incl. 1 GB read). proot = test-checklist reference only
   (GPL, ptrace — never copy).
5. **Installs**: parallel plan executor done; still to verify in VM: cancellation, fetch dedup, failure isolation.
6. **Smoke tests**: GTK4 (Text Editor), Kate (Qt), SuperTuxKart (SDL), an Electron app, Steam — on a clean disk.
7. **Build rules**: ISA baseline x86-64-v2 everywhere (kernel too; corei7 Rust patches approved); wrfsbase used when
   AT_HWCAP2 FSGSBASE, else arch_prctl. Multilib x86 libs, 64-bit zygote only; i386 driver parity with 64-bit.
8. **Later**: image size trim (3,286 MiB; 540 MB uncompressed .ko, erofs compression, Mesa strip); OTA server +
   installer per support policy (6 months free per version, free upgrade resets clock, extended to 4 y negotiable —
   releases 1–2 strictly 6 months); GLX minor gaps; real PC after.

Codex ladder: luna medium → luna-high → sol-6.1-low after 2 luna-high fails, max 1 sol job per 5 h.
