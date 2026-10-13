# Flatpak staging and verified publication

The current layout is [STORAGE-LAYOUT.md](STORAGE-LAYOUT.md). The existing
stage → install → generated-stub flow remains; the store-API redesign is deferred.

1. The bridge installs the per-device signed identity stub first. PackageManager
   assigns its UID. linuxd verifies the calling package/ref through the bridge's
   live authenticated identity callback; apps choose no paths or install flags.
2. `stage` prepares `/data/linux-app/staging/<uid>/<operation>`, hands its
   transient repository to the installing stub UID, and invokes the fixed
   `matonos-flatpak-store` helper. The helper clears supplementary groups and
   switches all UID/GID slots to that stub UID for network attribution.
   The wrapper validates the operation path/owner and selects that repository
   for both Flatpak roots during the no-deploy pull.
3. linuxd verifies pinned app/runtime commits, authenticated related refs and
   remote GPG/summary flags. `ostree pull-local --untrusted --gpg-verify` checks
   imported content before code objects are sealed with the verified-code type.
4. Runtimes/extensions go to the shared `/data/linux-app/runtime` installation;
   app code goes to a fresh `install/<appid>-<random>` version. Standard Mesa is
   selected from authenticated runtime GL metadata. Missing/invalid required
   dependencies fail the operation. RuntimeRefs durably protects selected refs
   before deployment and records the exact shared refs in each version.
5. Only successful publication atomically replaces `install/<appid>`. Previous
   versions remain while a stub holds their read-only shared lease. Runtime
   reconciliation merges retained versions before pruning unused shared refs.

Both roots are system-owned, system-wide installations. The current two-root
Flatpak invocation keeps `--user` for the app root and `--system` for runtimes;
these CLI selectors do not transfer ownership or create user-specific stores.
No runtime APK/UID, adoption rename, ownership marker, linuxd-owned user HOME,
per-user encrypted code trees or MLS home relabel is involved.

Uninstall is PackageManager stub removal for the selected user. Code is collected
only when no user retains a package at that app ID and no stub leases the version.
PackageManager removes private user data; linuxd never sweeps app homes.

The helper exposes only `stage` and `selftest`. Installer policy allows transient
repository access, network use and the narrow UID switch; no mount/loop/image
creation or app-data access. Build both helper and wrapper with
`tools/build-flatpak-helpers.sh`; it fingerprints the shared path header.

Host publication tests use a fake CLI to verify offline import/deployment order,
related refs, GL handling and failure propagation. Real signature verification,
Flatpak read-only launch/locking, encryption and enforcing SELinux need a fresh
coordinator-built image. No migration or arbitrary store limits are introduced.
