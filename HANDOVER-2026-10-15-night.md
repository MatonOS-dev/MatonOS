# Handover 2026-10-15 (night)

Started with "Chrome fails to install"; ended with a redesign of how Linux apps are installed and stored.
Nothing has been tested on a device since the morning. No new image has been built from main yet.

## State of the trees

- **main** (`5b574c0`, merge of `extra-data-installer`): installs moved into the Linux Subsystem app. Built? **No.**
  - `org.matonos.compositor` renamed to **`org.matonos.linux`** ("Linux Subsystem", `linux/subsystem/`).
  - Subsystem app runs Flatpak pull/deploy/`apply_extra` in staging, owns the store API
    (`ILinuxAppStore`, `LinuxAppStoreService`, `ConfirmInstallActivity`), stub generation/signing (own Keystore key,
    `allowClearUserData=false`) and stub self-install (caller cert must equal its own stub key).
  - Permissions: any app may request (subsystem's own confirm dialog); silent = `INSTALL_LINUX_APPS`
    (`signature|privileged`); list = `QUERY_LINUX_APPS`; remotes always confirmed unless privileged.
  - linuxd = install agent only: 10 commands (begin/verify/abort staged install, bind_pending, install_complete,
    install_failed, uninstall, reconcile_deployments, list_installed, installed_commit). `CAP_CHOWN` only, group system,
    no network. fs-verity sealing of staged files, no copy fallback. Errors: "Cannot trust /data, expected fs feature
    'fs-verity' isn't available" / "warning, subsystem app is misbehaving." (bridge force-stops the subsystem app).
  - `fsverity` added to both fstabs. SELinux: `matonos_linux_app` (app_domain + net_domain) for the subsystem app.
  - Bridge left the install path completely (−1.8k lines); window-geometry bridge deleted.
  - Removed: framework patch 0004 (client decorations; new approach next week), UID 2902, `matonos-flatpak-store stage`,
    legacy linuxd commands. 0004 **already reversed** in `/mnt/data/aosp/frameworks/base`.
  - Earlier today: `matonos-flatpak` prebuilt rebuilt (bwrap path, seccomp tmpfile under TMPDIR — fork commit
    `94702255` pushed to MatonOS-dev/flatpak, pin in MatonOS_apexs `f2d48d7`).
- **`ostree-packs`** (worktree `out/worktrees/ostree-packs`, 22 commits ahead of main, NOT merged): "option 0".
- **`installd-payload`** worktree: empty (fallback "option 2", installd patch — not started).

## Option 0 (current direction): Linux app payloads inside the stub APK

User-decided design (details in memory `compositor-runs-installs`):
- Payload = OSTree object store as **packs**: index in the stub, file objects as STORED, 4 KiB-aligned zip entries in
  **split APKs** named by content hash. Updates = new index + one pack of new objects, old packs inherited via
  `MODE_INHERIT_EXISTING` (hardlinked); repack packs whose dead ratio ≥ 0.5 (policy value).
- Compat proxy serves `/app` straight from the APK splits (`--pack-mount`): reopen carrier + seek, fd table,
  clamp/translate read/pread/lseek/mmap, fake stat, index-backed dirs, EROFS. 1× disk, no extraction.
- Loader maps segments file-backed (shared, reclaimable); anon+pread fallback only when a mapping is refused
  (also covers a future W^X tightening, b/237289679).
- Updates via a copy-on-write repo view for libostree (packs = read-only lower layer) — design only, not built.

Done on the branch (all host-tested; I re-ran the suites myself):
| Task | Commits | Status |
|---|---|---|
| 1 pack format + packer + C reader (real OSTree) | 9cddfeb, 6cd0239, 0c06745, 6436997, 31f739d | done |
| proxy APK-slice VFS (`pack-vfd`) | e82637e…b0b85c8 | done, proxy + pack suites pass |
| 4a Java pack writer, signed splits, launcher `--pack-mount` | 4182111…4bdbd43 | done |
| 4b on-device OSTree read (stock `ostree` applet), extra/ committed to private ref, artifacts after staging | f3e9fec…305a34d | done |
| loader file-backed maps + refusal fallback | e42df60, 494309e | done, suite passes |
| 4c PackageInstaller sessions | 262c187 (planning + tests only) | **blocked on a decision** |

## Open decision (ask the user first)

Android requires splits' `versionCode` == base. Recommendation sent, not yet answered:
**fixed `versionCode` + increasing `revisionCode`**, and move the **index into its own split** so updates are
split-only (`setDontKillApp(true)`, app keeps running on old pinned APKs, switches on next launch). Base only changes
when the stub itself changes. Brief to finish: `out/pc-logs/agents/pack-4c.md` + this decision.

## Next, in order
1. 4c with the decision above.
2. Runtimes as static-library APKs (same pack scheme; stub `uses-static-library`), check third-party install allowed.
3. `apply_extra` in an `isolatedProcess` (subsystem app becomes trust root for app contents).
4. COW repo overlay for updates; then retire linuxd's install job.
5. Merge `ostree-packs`, build ONE image, copy to `~/matonos/user-vm.img` (VM must be shut down; user asked not to
   build until option 0 is in). Device checks: list in `out/pc-logs/agents/pack-4c.final.md`; Firefox (plain) + Chrome
   (extra-data) installs; split hardlinks, split count, DONT_KILL_APP, static libs.

## Agents / tools notes
- Codex is now `gpt-6.1-sol` (config effort low). It did the proxy VFS that five `gpt-6-luna` runs failed. Briefs must
  be narrow; runs end at ~60–200k tokens. Allowed once: retry pack-vfd-type hard tasks with
  `-c model_reasoning_effort=high` if low stalls.
- Free opencode models: one job at a time (shared limits). DeepSeek v4.1 flash free did phase C well.
- Host now has `ostree` CLI + `gir1.2-ostree-1.0` (installed by the user today).
- Builds: `MATON_BUILD_COORDINATOR=1 ./tools/build.sh …` (guard rejects otherwise).
- Live APEX update on the VM fails (apexd dm EBUSY) and `adb remount` doesn't work — use hot bind-mount via
  `nsenter -t 1 -m mount --bind` for quick binary tests, or a new image.
- Follow-ups list: `out/pc-logs/agents/coordinator-followups.txt`.
