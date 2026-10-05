# r24 fork revision — bubblewrap 0.13.0

Upstream `v0.13.0`: `719a4fd474d44b26906bcf2b1b0fb6eddd8d56d0`.
Release archive SHA-256:
`4734237473c0e5d695e4e9034a34e43b2dbf5164655bd13fa59ae376b2b7a765`.

Local `upstream/` is a real Git checkout on `matonos/v1.2`, with the exact
release snapshot and an uncommitted bionic change. No new fork commit exists
(user instruction); review/commit/publication are outstanding. `source-pins.json`
pins every byte of the build tree independently of that upstream base commit.
A review snapshot is retained in ignored `build/deps-update/bubblewrap-fork.tar.xz`.

- Drop 0001 bool guard: upstream consistently uses stdbool.
- Keep/rebase 0002 getcwd: growing POSIX buffer, now guarded by `__BIONIC__`.
- Drop 0003/0004 realpath fallbacks: upstream replaced destination resolution
  with confined `openat2(RESOLVE_IN_ROOT)` / `safe_openat` fallback. No old
  ENOENT fallback is injected into the new security code.
- Add `chroot_realpath.c` and `safe_openat.c` to the Soong source list.
  `config.h` matches the NDK Meson define set: PACKAGE_STRING only; no
  SELinux/debug/assumed-kernel defines. Retain runtime kernel fallbacks.
  Upstream removed setuid support; no removed Meson option is passed.

Build with `MATON_AOSP=/path/to/aosp bash linux/third_party/bubblewrap/build.sh`
(maximum four jobs). The NDK binary stays in this worktree; image bwrap remains
a Soong source module. No shared product output or image was rebuilt.
