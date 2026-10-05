# Handover — 2026-10-05 late (Flatpak consolidation landed; image build running)

Supersedes the "In flight"/"Next" of HANDOVER-2026-10-05-evening.md. Read
`NOTES.md` → "Flatpak consolidation (2026-10-05)" for the design record.

`main` tip: `eb10e54` (MatonOS-dev/MatonOS, pushed). MatonOS_apexs tip:
`e7fe3de` (pushed). Working tree clean; `tools/preflight.sh` passes.

## Done (pushed)
- **Merged + pruned.** `static-flatpak-apex` and `codex/flatpak-works` merged
  into `main`; all merged/superseded worktrees/branches pruned (58 → 1).
  `codex/dbus-java-2` merged then dropped.
- **Bionic removed.** `retired/flatpak-bionic/` deleted; every active
  reference (README, device.mk, manifest, flatpak.mk, docs) updated. The only
  Flatpak is the static musl `com.matonos.flatpak` APEX.
- **Dispatcher fixed.** linuxd/store/wrapper agree on `argv[0]` applet
  dispatch; OSTree uses the multicall, not a removed `bin/ostree`. Verified by
  `install/linuxd/tests/flatpak-publish-host-test.sh` (all cases PASS).
- **Per-app installs, stock Flatpak.** linuxd resolves an app's `--user`
  install and the runtime app's `--system` install and runs uninstall/
  metadata/desktop_entry/icon/list_installed through stock Flatpak. The bridge
  injects `org.matonos.linuxruntimes`' UID for `add_flathub`/`list_remotes`.
  No global `/data/matonos/linux/flatpak*` installation remains.
- **Launch trimmed.** Generic `run` + caller run args removed; launch is
  stub-driven (signed stub ref + pinned commits).
- **Appstream stripped** from linuxd icon lookup; only the exported hicolor
  icon remains.
- **In-app dbus-java merged**: dbus-java-core 5.2.2 + Android LocalSocket
  transport, portal wire rewritten. `linux/compositor/host/test-portals.sh`
  passes. Native `linux/dbus-broker` is transitional, to be retired after
  device validation. No `xdg-dbus-proxy`.
- **Static APEX refreshed.** Full `build-static.sh` at MatonOS_apexs `e7fe3de`
  reproduced the same artifacts (SHA-256); helpers + prebuilts staged and the
  `linux/flatpak/prebuilt/static/SOURCE` pin updated. Stale
  `out/.maton-build.lock` cleared.

## Image build stopped to land flow B
The coordinator build was started (`MATON_BUILD_COORDINATOR=1 tools/build.sh -K
-M -j 16`) and then stopped during Soong analysis so the first-install flow
could be written into the tree; no image was produced. The stale
`out/.maton-build.lock` was cleared. Rerun that command after this lands.

## Not validated on device
Everything above is source-complete only. Nothing has booted since the static
APEX landed. MatonOS was never deployed — reset userdata, no migration.

## First device validation (after the image boots)
1. `/apex/com.matonos.flatpak` mounts and the five `bin/` files are static
   musl (`matonos-flatpak`, `matonos-bwrap`, `matonos-app-exec`,
   `flatpak-env-wrapper`, `matonos-flatpak-store`); no alias symlinks.
2. `flatpak --version` via the multicall; `org.matonos.linuxruntimes` is
   installed and its UID owns `/data/matonos/linux/apps/<runtime_uid>`.
3. `add_flathub`/`list_remotes` land in that runtime app's `--system` install
   (the bridge now passes its UID). Signed remote-add succeeds.
4. Install an app (store or `install/tools/test-flatpak.sh --install`), then
   confirm its deployment in `/data/matonos/linux/apps/<app_uid>` and launch
   it. Launch is stub-driven; no caller run args exist.
5. Uninstall and confirm app data under
   `/data/matonos/linux/apps/<uid>/home/.var/app/<id>` follows the
   `deleteData` choice.
6. dbus-java portal path: app D-Bus works, then retire `linux/dbus-broker`
   and its policy.

## Known open items
- **First-install flow B is implemented** (not device-validated): linuxd's
  `stage` command stages the signed app + runtime and returns the pins,
  `/metadata` and the exported desktop entry from the staged commit; the
  bridge builds the stub with the store's appstream icon
  (`FlatpakStubManager.installAsync`) and, once PackageManager installs it,
  deploys to the stub UID (`install`). `add_flathub`/`list_remotes` inject the
  runtime UID. The alternative install-then-rename flow (A) is documented in
  `install/linuxd/CODE-STORAGE-r24.md`.
- **dbus-java** 6.x is the eventual target (5.2.2 until it ships).
- **Local-only, unpushed branches:** `codex/zones`, `codex/gpu-props`,
  `claude/musl-static-116`, `ds/glibc-integrate`.
  `backup/pre-key-scrub-2026-10-04` stays local; never push it.
- **Architecture note.** The design keeps MatonOS close to stock: Flatpak is
  the sandbox assembler, linuxd only points stock Flatpak at per-app installs,
  and the runtime app owns the shared runtime installation.
