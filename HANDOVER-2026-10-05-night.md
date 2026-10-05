# Handover — 2026-10-05 night (static musl Flatpak stack)

Supersedes the "next steps" of HANDOVER-2026-10-04-evening.md. Decisions are
in Claude memory (matonos-flatpak-decisions, openpgp-checker,
musl-dns-forwarder); summary here.

## Decided (user, 2026-10-04/05)
- Flatpak runs from a fully static musl stack built from **Alpine 3.24
  stable** recipes: flatpak 1.16.6, ostree 2025.7, bwrap 0.12.0, merged into
  one multicall binary `matonos-flatpak` (flatpak/ostree/bwrap symlinks).
  Main's 1.18.4/0.13.0 pin gets lowered at integration.
- No GnuPG: **DullPGP** (gpgme-compatible verify/import on BoringSSL).
  No OpenSSL: curl (HTTP/HTTPS/FILE only) on the same pinned BoringSSL
  (AOSP external/boringssl 0cd1f6a). No xdg-dbus-proxy (global
  --socket=session-bus; the broker enforces policy). No triggers (user
  installs). No appstream/gdk-pixbuf (trim commit in the flatpak fork).
- Publish pipeline must enforce signatures via a verified `pull-local`
  (non-zero exit on bad sigs) — never `ostree show` (exits 0 on BAD).
- DNS: per-app forwarder in the stub at a uid-derived 127.x address,
  sock_diag uid check, android_res_nquery; apps stay in the host netns.
- Repos (public): MatonOS-dev/DullPGP (LGPL-2.1+), MatonOS-dev/MatonOS_apexs
  (Apache-2.0, APEX build files; Alpine bin/src as release assets, ~500 MB),
  MatonOS-dev/flatpak (only matonos/v26.10 = 1.16.6 + trim 03e6b20). The
  bionic-Flatpak forks were deleted by the user; local checkouts under
  linux/third_party/*/upstream are the only copies the current image builds
  from — keep them until the static APEX lands.
- Private keys: `*.pem`/`*.pk8` gitignored; the APEX key was scrubbed from
  history before the 2026-10-04 push. Old agent branches (e.g.
  ds/flatpak-apex) and backup/pre-key-scrub-2026-10-04 still contain it —
  never push them.

## In flight
- musl-324b (codex): curl FILE for pull-local, DullPGP default (no gpg),
  final locks/caches.
- static-apex-1 (codex): branch `static-flatpak-apex` (worktree
  out/worktrees/static-apex) — APEX plan, bionic → retired/flatpak-bionic/,
  new Android.bp, linuxd setup (CA symlink, /var/tmp, passwd/group),
  FORKS.md, pin lowering. Then image build + VM test (coordinator).

## Next
D-Bus Java port (dbus-java 6.x), then rework r24 pieces against the static
stack (publish pipeline, launch chain, DNS forwarder). Later: weekly
auto-update job on han-mc-server; a Flatpak test suite.
