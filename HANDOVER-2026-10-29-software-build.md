# Software Centre, title bars and builds — 2026-10-29

Entry point: [continuation handover](HANDOVER-2026-10-29-continuation.md). This summarizes the previous agent transcript; implementation claims below were reported by that agent unless stated otherwise.

## F-Droid Source: merged, device testing pending

| Device-tree commit | Delivered |
| --- | --- |
| `b156852` | Shared Source interface, FlathubSource and read-only FdroidSource; merged search results with source badge; entry.jar signature checked against pinned official fingerprint, index-v2 JSON hash checked. |
| `5600a5c` | Verified APK downloads, install/uninstall with progress, split APK sessions, Installed integration, privileged install/delete permissions. Earlier `1aed499` references identify pre-merge work. |
| `da6f600` | Updates only for older installed versions with matching signing certificate; shared Updates view; WorkManager background checks and unattended updates for apps installed by the Software Centre. |
| `64b6276` | User repositories, fdroidrepos:// links, host/fingerprint trust prompt, punycode host display, failed verification shown as broken, removable user repositories, official repository protected. Documentation rewritten. |

Host Java/security tests, TypeScript, lint and signed builds were reported passing across phases. Node 24 versus specified Node 22 produced a warning. Early preflight failures were attributed to unstaged build artifacts/missing bpfmt; they were not device validation. No device test of F-Droid install permissions, installation, updates, background behavior or repository management was completed. Permissions require the next image; installing only an updated APK is insufficient.

Implementation reference: [docs/SOFTWARE-CENTRE.md](docs/SOFTWARE-CENTRE.md). Worktree `/home/hanro50/matonos/repos/dt-fdroid` is now on `title-bars`, not an unfinished F-Droid branch.

Next image acceptance checks: official index verification; positive/negative signature/fingerprint/hash handling; merged search; APK and split APK install/uninstall; Installed and Updates views; signer mismatch rejection; background checks; unattended update eligibility; user-repo trust/add/remove/broken states. Confirm actual installed privilege grants and package signer behavior on-device.

## Custom title bars: design only

Design commit `f4302eb`, merged via `aa702e0`; [docs/TITLE-BARS.md](docs/TITLE-BARS.md). Use a Car-style MatonSystemUI module linking stock SystemUI, supplying our own WM Shell decoration implementation. Caption suppression and client-decorated Linux title-bar drag go through the System Bridge; no framework patch is planned.

No implementation or Soong boot check was completed. Step 1 is a minimal SystemUI override/skeleton and boot verification, followed by decoration integration, Linux caption/drag support and interaction checks. Read the design for exact sequencing. **User explicitly deferred title bars until Steam works and rejected stopping the VM during critical i386 work.** Do not treat the earlier proposal to alternate VM stops and builds as approval.

## Build recovery and Flatpak provenance

The previous agent deleted old pc-logs with permission, accidentally deleting build dependencies too: musl-324 and erofs-pipeline lived under the log directory. This broke the subsystem build. Recovery `026e3dd` moved scratch under `/home/hanro50/matonos/build/`; Alpine root is `musl-324`.

Recreation script in subsystem repo: `linux/flatpak/build-support/setup-scratch.sh`. It recreates the Alpine 3.24.2 root, packages, pinned Flatpak checkout and verified source cache. It was run twice successfully. The initial dependency rebuild then failed because gettext-static/libintl.a was absent; `c7cd657` added the package and the dependency rebuild passed. Generated Meson/Flatpak plan caches retaining deleted paths also needed targeted regeneration.

Flatpak is built from the MatonOS-dev fork pin `8078df14…` in sources.lock, not Alpine's packaged Flatpak. `97ada32` removed stale unused FLATPAK_COMMIT=03e6b205… from alpine-enter.sh. `c7cd657` also inadvertently committed a rebuilt matonos-flatpak via commit -a; the previous agent reviewed it and kept it because it came from the pinned fork and was running in the working apps. Do not restore an old binary merely to match its former hash.

Normal subsystem build (run from subsystem repository; private signing files stay local):

```bash
set -e
export MATON_AOSP_ROOT=/mnt/data/aosp
export MATON_SUBSYSTEM_OUT="$HOME/matonos/build/main-out"
export MATON_SIGNING_STORE_FILE="$HOME/.config/matonos-keys/matonos-linux-subsystem.jks"
export MATON_SIGNING_PASSWORD_FILE="$HOME/.config/matonos-keys/matonos-linux-subsystem.pass"
./build.sh
```

Use `--build-dependencies` when the build actually requires dependency recreation. A successful build produces MatonLinuxSubsystem.apk and its bootstrap APK. Check the complete exit status before installation. Continuation build of `1dbec2a` passed; `/tmp/maton-build-r61-codex.log` is temporary evidence and should not be assumed permanent.

## Operating rules and lessons

- Read AGENTS.md in both repositories and paths.md. Current user decisions override old chronological notes. Google-certifiable Android > performance > compatibility > simplicity.
- No VM stop/Soong build for title bars until Steam works. Never overwrite an image attached to a running VM. Preserve the user's VM on 5555.
- Build with available CPUs; the current no-job-cap rule supersedes older hard-coded worker notes.
- Keep scratch outside pc-logs. Before deleting any directory, inspect build-script references and active consumers. No additional cleanup is required by this handover.
- Historic captures day-r4x–r59 were moved to /mnt/data2/matonos-captures and subsequently deleted with permission. Many older evidence paths in the transcript no longer exist. day-r60–r62 were retained during the first cleanup; verify any evidence path before relying on it.
- Never reset dirty worktrees. Preserve another worker's edits and pending commits. Wait on known PIDs; pgrep -f can match the calling shell and create an endless waiter.
- Do not modify running shell scripts. Avoid broad AOSP searches during builds. Keep source, generated artifacts and private keys clearly separated.
- Model quotas and credit statements in the transcript were time-specific. Check current availability rather than restarting obsolete scheduled jobs.

After handing over, clear context before starting a new task, as required by this device tree's AGENTS.md.
