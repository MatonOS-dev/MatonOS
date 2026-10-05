# Handover — 2026-10-05 late (Flatpak consolidation landed)

Supersedes the "In flight"/"Next" of HANDOVER-2026-10-05-evening.md. Read
`NOTES.md` → "Flatpak consolidation (2026-10-05)" for the design record.

## Done (pushed)
- **Merged + pruned.** `static-flatpak-apex` and `codex/flatpak-works` merged
  into `main`; all merged/superseded worktrees/branches pruned (58 → 1).
- **Bionic removed.** `retired/flatpak-bionic/` deleted; every active
  reference (README, device.mk, manifest, flatpak.mk, docs) updated. The only
  Flatpak is the static musl `com.matonos.flatpak` APEX.
- **Dispatcher fixed.** linuxd/store/wrapper now agree on `argv[0]` applet
  dispatch; OSTree uses the multicall, not a removed `bin/ostree`.
- **Per-app installs, stock Flatpak.** linuxd resolves an app's `--user`
  install and the runtime app's `--system` install and runs uninstall/
  metadata/desktop_entry/icon/list_installed through stock Flatpak. The bridge
  injects `org.matonos.linuxruntimes`' UID for `add_flathub`/`list_remotes`.
  No global `/data/matonos/linux/flatpak*` installation remains.
- **Launch trimmed.** Generic `run` + caller run args removed; launch is
  stub-driven.
- **Appstream stripped** from linuxd icon lookup.
- **In-app dbus-java merged** (`codex/dbus-java-2` → `main`): dbus-java-core
  5.2.2 + Android LocalSocket transport, portal wire rewritten. Native
  `linux/dbus-broker` is transitional, to be retired after device validation.
  No `xdg-dbus-proxy`.
- **Static helpers rebuilt** (`stage-static-musl.sh`); prebuilts + `SOURCE`
  refreshed. Helper sources synced to MatonOS_apexs `e7fe3de` (pushed).
  Stale `out/.maton-build.lock` cleared. `tools/preflight.sh` passes.

`main` tip: `c4774a9` (MatonOS-dev/MatonOS). MatonOS_apexs tip: `e7fe3de`.

## Not validated on device
Everything above is source-complete only. Nothing has been booted since the
static APEX landed. MatonOS was never deployed, so there is no migration to
do — reset userdata.

## Next
1. Refresh the static APEX pin: run the full `MatonOS_apexs/flatpak/
   build-static.sh` (via `alpine-enter.sh`) so the multicall binary, helpers
   and licenses match the vendored sources, then re-stage and confirm
   `linux/flatpak/prebuilt/static/SOURCE`.
2. Build the image (`tools/build.sh -j 16`) and boot-test: APEX mounts,
   `flatpak --version`, signed remote-add, install, launch, uninstall, app
   data under `/data/matonos/linux/apps/<uid>/home`.
3. Validate the dbus-java portal path; then retire `linux/dbus-broker` and
   its policy.
4. Finish the Software Centre install flow (the store's `installApp` still
   sends only `{ref}`; linuxd needs the signed pins/UID from the stub).

## Open / housekeeping
- Local-only, unpushed branches: `codex/zones`, `codex/gpu-props`,
  `claude/musl-static-116`, `ds/glibc-integrate`. `backup/pre-key-scrub-
  2026-10-04` stays local; never push it.
- dbus-java 6.x is the eventual target (5.2.2 until it ships).
