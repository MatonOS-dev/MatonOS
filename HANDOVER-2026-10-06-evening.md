# Handover 2026-10-06 (evening)

Main thread today: Flatpak installs through the store, and the Flatpak stack
moved from musl to bionic. Details: `docs/FLATPAK-STUB-HANDOVER.md`.

## Done (all pushed to main)

- **Bionic Flatpak APEX.** MatonOS-dev/MatonOS_apexs `flatpak/` replaces the
  musl recipe (reproducible fetch.sh/build.sh, pinned inputs). New repo
  MatonOS-dev/bionic-fill (MIT shim); MatonOS-dev/flatpak `matonos/v26.10`
  force-pushed (AppStream/FUSE/gdk-pixbuf removed, Flathub User-Agent fix).
  Verified on a VM as an app UID: Flathub remote, DNS via netd, TLS via
  Conscrypt, DullPGP signatures, Calculator installed.
- **Store-driven install:** hidden stub → "install me" activity → bridge →
  per-stub-UID staging (codex/installer-staging merged, ownership reworked).
- Bugs fixed on the VM: wrapper TMPDIR (Android /tmp), corrupt stub string
  pools + unaligned APK entries (no generated stub could ever install),
  disabled-launcher metadata lookup.
- **ccache on by default** (coord-build.sh); content-hash settings in
  ccache.conf. Incremental image builds ~15 min.
- Preflight skips node_modules/.gradle/.cxx (it hung walking them).

## In flight / next

- Image with c6bad78 + 9cda540 (per-UID staging) was building: test a full
  store install on the VM next (recipe in the Flatpak handover).
- Keep the stub in the foreground during installs (background firewall).
- Decisions open: replace the dead C D-Bus broker (still the session bus);
  SELinux + `zones` are next week.
- Later: build hot parts (out/soong, siso state) on the NVMe (measure first).
