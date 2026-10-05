# Flatpak staging and verified publication

Flatpak installation uses stock Flatpak deployments. The privileged helper
`matonos-flatpak-store` has exactly two commands: `stage` and `selftest`.

For `stage`, linuxd passes the fixed `/data/matonos/linux/staging` path, a
configured remote, and a validated install ref. The helper clears supplementary
groups, switches to AID 2902, sets `MATON_FLATPAK_STAGING_DIR`, and execs the
fixed APEX Flatpak wrapper with `install --system --no-deploy`. Flatpak checks
the remote's configured commit and summary signatures while pulling into the
shared staging repository. The helper cannot choose installation options or
write outside that repository.

linuxd checks the staged refs against the requested commit pins and verifies
that both GPG checks are enabled. It then uses signature-verifying
`ostree pull-local` into the target repositories and runs
`flatpak install --no-pull` to publish the deployments. The staging directory
uses `matonos_flatpak_store_file`; that type grants the installer only staging
repository access and grants linuxd read access for verification.

`matonos_flatpak_installer` retains network access, setuid/setgid for AID 2902,
the APEX Flatpak wrapper execution, shared staging repository writes, and the
inherited descriptor needed to report the child result. It has no mount, loop,
filesystem relabel, fs-verity, image creation, or `sys_admin` permissions.
The stage helper does not create or mount filesystems.

The host publish harness remains:

```sh
bash install/linuxd/tests/flatpak-publish-host-test.sh
```

The harness checks good signed refs, bad pins and corrupted objects. Set
`MATONOS_PUBLISH_RUN_LAUNCH_TEST=1` to include the launch-chain probe.

## First install: the app UID is late-bound

The stub APK is what receives the Android UID, so a first install has no app
UID yet. Only the shared runtime `--system` installation is known up front (it
is owned by the preinstalled `org.matonos.linuxruntimes`). The bridge passes
the calling software store's UID as the **installer UID**. Two workable flows:

### A. Install first, then rename to the stub UID
1. `install` publishes the runtime to `/data/matonos/linux/apps/<runtime_uid>`
   and the app to `/data/matonos/linux/apps/<installer_uid>` (`--user`).
2. The bridge reads that deployment's desktop entry/icon and installs the
   stub; PackageManager assigns the stub UID.
3. `adopt` renames `/data/matonos/linux/apps/<installer_uid>` to
   `/data/matonos/linux/apps/<stub_uid>` and re-homes it: ownership, MLS
   categories, `<uid>.owner` record, runtime-installation record.

Reuses today's post-install desktop/icon extraction; the cost is the rename
plus relabel, and the app briefly lives under the store's UID.

### B. Stub first, then install directly to the stub UID (preferred)
Verified against a real staged Flathub commit: it carries `/metadata` and
`/export/share/applications/<id>.desktop`, but often **no exported PNG** under
`/export`. So the icon comes from the software centre's Flathub appstream
(already fetched by `rn-apps/flathub/src/FlathubApi.ts`), not the deployment.
1. `stage` (new): linuxd stages the signature-verified commit and returns the
   pinned commits, `/metadata` and the exported desktop entry read from the
   staged commit (`ostree cat <commit> /metadata`, etc.). No deploy yet.
2. The bridge maps `/metadata` to Android permissions
   (`StubGenerator.permissionsForMetadata`), takes the icon from the store,
   and installs the stub; PackageManager assigns the stub UID.
3. `deploy` (new, split out of publish): hardlink the runtime into
   `/data/matonos/linux/apps/<runtime_uid>` and the app into
   `/data/matonos/linux/apps/<stub_uid>`, then record the runtime install.

No rename or re-home, and the app is owned by its stub UID from the start.
Requires the store to hand the icon (and display name) through the bridge
before deploy.

Both flows keep one owner per install, hardlink the runtime from the shared
staging repo, and leave the runtime/system side unchanged. Updates to an
already-installed app use its stub UID directly (no adopt step).
