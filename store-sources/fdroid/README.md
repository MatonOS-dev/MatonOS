# F-Droid store source

`org.matonos.store.fdroid` is a Java-only, API 37+ backend for the MatonOS Software Center. It has no activity or installer privilege. Its exported service requires the store's signature bind permission and implements the AIDL contract in `store-sources/contract/` (SOURCE-PLUGINS.md). `info()` advertises `HAS_REPOSITORIES`; reviews are empty.

The original Java index client, repository validation, JSON reader, SHA-256 verifier and host test moved from the store module. Repository preferences and installed-package signer/version inspection were ported from Kotlin to Java. All 17 preset URLs and signing fingerprints are preserved, as is the official fingerprint. Presets seed once; the official repository cannot be removed. Additions require a signing fingerprint in the share URL. Failed verification leaves the repository visibly broken and logs the cause; removing a preset keeps it removed.

Each catalogue authenticates `entry.jar`, pins its certificate, then checks the signed index SHA-256 before parsing or caching. Listings use package IDs and repository URL origins. Details include final anti-feature and safety objects, ported from the store's TypeScript mapping. Across working repositories the highest compatible version wins; SDK requirements, native ABIs and required features filter versions. Installed listings and updates require matching current signing-certificate sets. Exact signer matching deliberately retains the existing behavior (certificate rotation lineage is not supported).

Install immediately starts background work. It selects a compatible release, downloads every APK, checks all SHA-256 values and package/split identities, opens read-only descriptors and calls the store's `commit` exactly once with `inherit=false` and no removed splits. Only the commit-result callback reports success/failure. Local descriptors and temporary APKs are removed after Binder transfers their copies. Standalone split names are read from binary manifests because PackageManager's archive parser expects a base APK. This backend never calls PackageInstaller.

The store discovers enabled plugins once per session and refreshes on explicit toggles. Repository buttons require `HAS_REPOSITORIES`. F-Droid-specific Expo functions and the unused periodic worker are removed. The generic package-result receiver is now `SourceInstallReceiver`; it remains for plugin uninstall results. The generic commit path owns its input descriptors without double-closing them.

## Build and image integration

`buildinfra/apps/apps.list` registers `matonos-fdroid-source`; the existing `tools/build-apps.sh` builds and signs it with a persistent user-local key under `~/.config/matonos-keys/`, outside git. An SDK containing API 37 is required (this machine has `platforms/android-37.0`). Use:

```sh
MATON_BUILD_COORDINATOR=1 MATON_AOSP_ROOT=/mnt/data/aosp \
  MATON_APPS_ONLY=matonos-fdroid-source bash tools/build-apps.sh
```

The staged `buildinfra/apps-built/FdroidSource.apk` is imported by `apps/Android.bp` as `MatonFdroidSource` and selected by `apps/apps.mk`. It is an ordinary product system app, enabled by default through existing plugin discovery. Its only requested permissions are INTERNET and QUERY_ALL_PACKAGES. No private key or APK is committed. The app registry's existing certificate-generation step also lists every app certificate for the bridge; this plugin requests no bridge permission or capability.

## Verification

- `bash store-sources/fdroid/test-host.sh`: signed JAR and tampering, signer pin, index hash, split file parsing, SHA-256 rejection, update signer/version eligibility, URL/fingerprint validation, compatible version choice, safety mapping, and real aapt2-produced base/split manifest names and package mismatch rejection.
- Gradle `assembleDebug` and signed `assembleRelease` via `tools/build-apps.sh`.
- In `rn-apps/flathub`: `npx tsc --noEmit -p .` and `npx eslint src`.
- Store signed release build and release linkage check passed; the final store module compiled. Preflight passed all six checks with the host bpfmt on PATH and read-only links to existing unrelated prebuilts in this isolated worktree.

No image build, QEMU boot or device installation is authorized for this task. End-to-end Binder transfer, PackageInstaller confirmation and default enablement still need a fresh image test. Public repositories have not been exercised by these fixture tests. Repositories are checked sequentially with bounded network timeouts; a slow/offline preset can delay catalogue calls. If every repository fails, the call fails loudly; partial failures are logged and marked broken while other repositories remain usable. Large queries can still hit Binder's transaction-size limit; pagination would require a future contract revision.

## Fresh-image checks (all supported PCs)

1. Have the coordinator build a fresh image containing both rebuilt apps. Boot it on a Ryzen/RX6600 PC, Surface Pro 3 or HP ProDesk; connect networking. No hardware-specific plugin behavior is required.
2. Open Sources: F-Droid source is enabled without a user toggle. Search for an app, open details and confirm source, repository, permissions and anti-feature/safety data. Disable the plugin and verify its results and repository button disappear; re-enable it.
3. Open its repository list. Check the official entry and all presets. Remove one preset, restart the store/plugin and confirm it stays absent. Attempt official removal through the service and confirm rejection.
4. Add a repository share URL with its independently obtained fingerprint. Confirm valid verification; reject a missing fingerprint and mark a wrong fingerprint/offline repository broken. Check logcat tag `FdroidSource` for contextual errors. Open an `fdroidrepos:` link and verify it only opens repository review.
5. Install a small app; confirm progress, one store-owned full-install session and eventual completion. Repeat with a base plus splits, then update to a higher matching-signer version. Verify installed package/splits and empty temporary download cache. Corrupt a fixture APK/hash and confirm no commit is sent.
6. Uninstall through the store and confirm the generic receiver completes the operation. Test an offline install and denied/cancelled package confirmation: one failure result, no stale busy indicator. Verify the plugin itself has no install/delete privilege.
