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
