# Handover — 2026-10-05 evening (static Flatpak stack, enforcing boot, store plan)

Supersedes the "Next" sections of HANDOVER-2026-10-05-night.md. Decisions
live in Claude memory (matonos-flatpak-decisions, openpgp-checker,
musl-dns-forwarder, no-gplv3-locked, software-centre, security-zones,
gpu-detect-properties, policy-fast-check); summary below.

## State of main (pushed)
- F-Droid Basic REMOVED from the image (GPLv3 rule: no (L)GPLv3 in anything
  the user cannot replace). Source in `retired/fdroid-basic/`.
- `docs/SOFTWARE-CENTRE.md`: design of the MatonOS Software Centre — one
  backend API, three Sources (APK via F-Droid-format repos incl. custom
  repos, Flatpak incl. custom remotes, MatonOS APEX from our signed channel).
  Replaces the updater planned for the system settings app. F-Droid's client
  is GPLv3 (not reusable); its `libs/` (index/download/database) are
  Apache-2.0 per upstream — verify full dependency licences before use.
- `make-live.sh`: new boot entry **"MatonOS Live (debug-permissive)"** (debug
  + SELinux permissive) for collecting denials; installed UKIs always enforce.
- Fixes: AID 2902 name (<32 chars), bridge manifest description, live
  RAM-disk labels (enforcing boot loop).
- The bionic Flatpak fork checkouts (`linux/third_party/*/upstream`) are
  DELETED (GitHub repos deleted by the user; backup
  `~/matonos/backup/bionic-flatpak-upstreams-2026-10-05.tar.zst`). **main can
  no longer build the bionic Flatpak APEX — `static-flatpak-apex` must land.**

## Integration branch `static-flatpak-apex` (pushed, NOT merged)
Contains: static musl APEX (every `bin/` file static musl, ≈10.8 MB:
matonos-flatpak 10.56 MB multicall flatpak/ostree/bwrap with DullPGP + curl
on BoringSSL, no OpenSSL/GnuPG/libidn2/libunistring/libpsl; launcher, store
helper (stage-only), matonos-bwrap, matonos-app-exec 39–84 KB each); argv[0]
dispatch (Soong drops APEX symlinks); publish pipeline (verified
`pull-local --untrusted --gpg-verify`); launch chain (global
`--socket=session-bus`, `--no-a11y-bus`); per-app DNS forwarder; cleanup
(RUN_DOWNLOADED_CODE + mount helper removed); licence gate + notices;
enforcing-boot policy (5 permissive-collected packets).
- Image build **#16 OK** (16:02). Enforcing VM test: storage + all APEXes
  fine; **SurfaceFlinger still aborted** because `pc-gpu-detect` crashed
  (denied `search` on `sysfs_gpu` dirs). Fix `319250c` (+ modprobe fd use,
  bootanim memfd) is on the branch; validation via
  `tools/build.sh -m selinux_policy` was still running at 16:31 (it did a
  full Soong analysis — the first `-m` run re-analyses; later ones should be
  minutes). **Main checkout is detached at 319250c** — restore with
  `git -C device/maton/pc_x86_64 checkout main` after that check.
- Merge criterion (user): enforcing boot to boot_completed + on-device
  Flatpak flow (signed remote-add, verified publish, launch a CLI app) on the
  same image → then merge into main and push. Not reached yet.

## Other pushed branches (review)
- `codex/gpu-props` (0d83361, on top of 319250c): pc-gpu-detect publishes
  `vendor.maton.graphics.*` during early-init; QEMU-specific genfscon lines
  removed (they would break enforcing on real PCs). Merge into the
  integration branch next, then image + VM.
- `codex/enforcing-boot`, `codex/avc-{graphics,audio,flatpak,bridge,core}`,
  `codex/publish-pipeline`, `codex/launch-chain`, `codex/dns-forwarder`,
  `codex/cleanup-obsolete`, `codex/store-strip` — all merged into the
  integration branch already.
- `codex/dbus-java-2`: dbus-java **5.2.2** (user: until 6.x ships) + own
  LocalSocket transport; device-test the `com.sun…UnixSystem` reference.

## Repos
- MatonOS-dev/DullPGP (LGPL-2.1+): security fixes, fuzzing, enforcement test.
- MatonOS-dev/MatonOS_apexs (Apache-2.0): build files, locks, licence gate,
  static helpers; Alpine 3.24 bin/src as release assets.
- MatonOS-dev/flatpak: only `matonos/v26.10` (1.16.6 + trim).

## Next (in order)
1. Finish the `-m selinux_policy` check of 319250c; merge `codex/gpu-props`;
   image build; enforcing VM test (normal debug entry) → if both criteria
   pass, merge into main.
2. Queued briefs in `out/pc-logs/agents/`: `zones.md` (security-zone model in
   CLAUDE.md + our own boundary neverallows + `matonos_hw_reader`),
   `policy-check.md` (visibility script + neverallow pre-check; do it).
3. Real hardware: boot "MatonOS Live (debug-permissive)" on the build PC and
   Surface Pro 3, collect denials (packet method), fix, then enforcing.
4. Mesa licence audit; label the two unnamed `default_prop` /
   `vendor_default_prop` property reads; kernel memfd/tmpfs labelling
   (several rules exist only because memfds arrive as generic tmpfs).
5. Later: Software Centre implementation; weekly auto-update job on
   han-mc-server; Flatpak test suite.

## Lessons
- Validate policy with `tools/build.sh -m selinux_policy`, not image builds
  (six full rebuilds today were one-line policy errors: private types in
  vendor policy, vendor types in system_ext, unknown `dns_port`, stock
  neverallows).
- Never start a build while another coord-build is alive (build.sh continues
  with a kernel/AOSP pass after a ninja failure).
- No slot/BDF-specific genfs labels; graphics clients read properties.
- Old agent branches made before the 2026-10-04 key scrub still contain the
  APEX private key: never push them.
