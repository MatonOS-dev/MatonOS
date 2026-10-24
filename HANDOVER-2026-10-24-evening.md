# Handover 2026-10-24 evening (19:15)

Deadline: **Sunday 2026-10-25 evening = pre-alpha.** User: aim to ship the **mount APEX path** in it ("I believe in
you"). Codex quota nearly exhausted (resets ~03:42); sol allowance 2 jobs / 5 h (none used since 14:18).

## Big decisions today (all in memory + subsystem AGENTS.md)
- **Priority chain:** Google-certifiable Android > performance > compatibility > simplicity.
- **File access = real mounts only (path B).** A privileged `mountd` in an **updatable APEX** bind/loop-mounts each
  app's full Flatpak view; when mounted, the proxy's seccomp filter **does not trap file syscalls** (native speed).
  Seccomp can't see paths, so the mounted view must be complete; nested views (Steam pressure-vessel via
  `org.freedesktop.portal.Flatpak` `Spawn` usr-fd/app-fd) = **phase 2** (`MOUNT_SUBVIEW`, new mount ns per child).
- **No translation fallback.** Missing/mismatched APEX → loud error, Linux app does not start. The proxy VFS
  translation code is **deleted after phase 2** works.
- **Translation path archived:** tags `archive/translation/{site-rewrite,site-rewrite-1b,site-rewrite-2,rewrite-i386,
  proxy-launch,proxy-fdbook}` (pushed). Rewrite code **reverted** from subsystem main (backup tag
  `archive/main-before-rewrite-revert`). riscv64 dropped (supported: x86_64, i386, arm64).
- **Packs = EROFS images** stored STORED + 4096-aligned in APK splits, loop-mounted at an offset (APEX-style),
  SHA-256 in signed split metadata. `mkfs.erofs` (GPL-2+) may ship in the APK **only as a separately executed binary**.
- **APEX ↔ subsystem tightly bundled:** lockstep versions, versioned protocol; subsystem bundles the APEX and installs
  it when the active one is older; errors if missing.
- **Trust model (final):** subsystem app and mountd APEX share one signing key → paired. The paired subsystem pins the
  **stub public key** in mountd (PAIR_PIN). **Stubs call mountd directly**; mountd verifies the stub APK signer
  against the pin + pidfd PID↔UID↔package. **Re-pin only from the subsystem app**, no user prompt. Factory reset
  clears the pin.
- **Upstream reuse:** upstream `flatpak-portal` runs per stub (merged). `flatpak-session-helper` queued. **Never ship
  xdg-desktop-portal** (one bus per app). Keep our in-stub D-Bus broker; remove its own/talk name filter
  (brief `agents/bus-filter.md`). Future: intents over D-Bus, cross-app via binder.
- **Loud layer:** never hide the compat layer; layer-caused crashes reported as **MatonOS crashes**; compat portal
  `org.matonos.CompatLayer` queued (`agents/compat-portal.md`).
- **i386 builds:** `-m32 -march=x86-64-v2`. Assembly welcome on measured hot paths.
- Steam **paused** until the mount system is done (resume with phase-2 nested views).
- Deferred: integrity-sensitive (anti-cheat) mode, per-app compat toggle, at-spi2 → Android a11y bridge, arm64.

## Repos / state
- Subsystem `~/matonos/repos/linux-subsystem-app`: pushed **9b269f9** (revert + EROFS packer + guard allowlist).
  Local main **692fa92** = + steam5 fixes (portal Spawn via packaged Flatpak CLI, directed D-Bus signals to unique
  names, proxy flock fds ≥ 512) — **not pushed: its build (`~/matonos/build/main-out/m6.log`) failed; cause not yet
  checked** (last time this was the Flatpak prebuilt ISA audit → `./build.sh --build-dependencies` JOBS≤4, then
  `tools/refresh-flatpak-prebuilt-source.sh`, commit prebuilt, rebuild).
- Device tree (here): `main` clean at 6bb935b (+ this handover). Branch **`mountd-apex`** = 4920bb2 (parked mountd
  prototype: daemon builds, selinux_policy unchecked) + c2bcdb6 (agent WIP: protocol draft, DESIGN/README updates).
  `wip/mountd-dev2` = older parked copy.
- Worktrees: `lsa-trim` (erofs-pipeline, running), `lsa-gpuint` (branch mounts-subsystem prepared, clean),
  `lsa-dns` (steam5, merged), `lsa-i386` (revert-rewrite, merged — removable).

## Running / scheduled
- `erofs-pipeline` (luna-high, lsa-trim): static `mkfs.erofs` (exec only) + install pipeline producing EROFS splits.
- `relaunch-mounts-sub.sh` starts **`mounts-subsystem` (sol) at 20:43**: APEX bundling/update + error, PAIR_PIN,
  MOUNT_VIEW of the full Flatpak view, untrap filter, fake mountd for host tests. Brief `agents/mounts-subsystem.md`.
- **`mountd-apex` stopped** (device side). Brief `agents/mountd-apex.md` (luna-high per user; DeepSeek paid review of
  security afterwards). Resume from branch `mountd-apex`. Shared protocol draft `agents/mountd-protocol.md`.
- VM: persistent disk VM at 127.0.0.1:5556 (v46) running, started detached by the coordinator.

## Next (in order)
1. User's last instruction before this handover: **add a stub mountd APEX to the image** (preinstalled placeholder so
   the real APEX can be installed later as an update — apexd only updates preinstalled APEXes), then **build image
   v47** while codex is low. VM must be OFF for Soong (`adb shell sync`, `out/pc-logs/agents/vm.sh kill 5556`).
   No APEX exists in the device tree today (the old com.matonos.flatpak lives in ~/matonos/repos/MatonOS_apexs).
   Keys: no private keys in the public repo; shared subsystem/APEX signing key decision applies.
2. Fix + push subsystem local main (m6 build failure).
3. mountd-apex resume (device side), mounts-subsystem (sol 20:43), erofs-pipeline → integrate → v47/v48 VM test of a
   mounted Tux Racer launch with untrapped file syscalls.
4. Then phase 2 (nested views) → Steam resume → delete translation code.

## Gotchas learned today
- Agents read "no commit -a" as "don't commit" → common.md now says COMMIT explicit paths.
- VMs started by agents die when the agent session ends → coordinator starts the disk VM with `setsid nohup`.
- Use `watch-finish.sh <pids>` (agents dir) to get notified; I missed finished agents twice.
- Never pattern-kill; `pgrep -f` matched my own shell once more — use `ps -eo pid,args | grep` + literal PIDs.
- Editing a running relaunch script doesn't affect it (sed -i makes a new inode) — kill + restart the scheduler.
- Merges that refresh the Flatpak prebuilt can trip the ISA audit in other out dirs: rebuild deps in that out dir.
- DeepSeek free has its own usage limit; paid OpenRouter DeepSeek (`openrouter/deepseek/deepseek-v4-flash`) available.
