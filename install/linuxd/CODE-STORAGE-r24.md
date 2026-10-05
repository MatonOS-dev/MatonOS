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
