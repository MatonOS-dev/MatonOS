> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Software Center — F-Droid phase 1

The app has a shared TypeScript `Source` contract for search, details,
installed apps, updates, install, uninstall, and progress. `FlathubSource`
wraps the existing Flathub API and Flatpak bridge operations. `FdroidSource`
reads the built-in official F-Droid repository catalogue and marks install,
uninstall, and updates as unsupported in this phase. Search merges both
catalogues into one list with a visible source badge.

The native Java index client downloads `entry.jar`, verifies its JAR signature
and signer fingerprint, reads `entry.json`, then verifies the SHA-256 of
`index-v2.json` before parsing or caching it under the app's private files
directory, separated by repository URL. The signer fingerprint is pinned from
[F-Droid's release signing keys documentation](https://f-droid.org/docs/Release_Channels_and_Signing_Keys/).
The catalogue parser and client are original code based on F-Droid's published
index format; no F-Droid client code is included.

## Phase 1 verification

- `modules/matonos-flathub/android/test-host.sh` runs host-Java checks for a
  valid signed JAR, a damaged signature, a wrong signer pin, an index hash
  mismatch, and parsing app/version/APK metadata from a fixture.
- `npm run typecheck` and `npm run lint` check the TypeScript app.
- `MATON_AOSP_ROOT=/mnt/data/aosp MATON_APPS_ONLY=matonos-flathub tools/build-apps.sh`
  builds and signs the Expo app, then checks release linkage.
- A fresh image/VM was not used for this read-only catalogue phase. On a
  network-connected MatonOS image, search for a known package and verify it
  appears beside Flathub results with the F-Droid badge; open its details and
  confirm the version and APK metadata. F-Droid rows identify themselves as
  read-only and have installation disabled.

Phase 2 needs verified APK download and `PackageInstaller` install/uninstall
sessions, source-aware progress, installed-package reconciliation, and tests
for APK hash and signer checks. Updates remain intentionally unsupported.

---

Historical Flathub behavior and earlier verification notes follow.

The app began as a Flathub-only UI prototype. The target shared APK,
Flatpak design is in
[`docs/SOFTWARE-CENTRE.md`](../../docs/SOFTWARE-CENTRE.md); this prototype's
direct Flathub API and existing Flatpak bridge calls are not the final
multi-Source contract.

An Expo app for browsing Flathub's public v2 API and installing system Flatpaks
through MatonOS System Bridge. The UI uses Expo UI's Jetpack Compose primitives.
It has Browse (popular and recently added), search, app details, and Installed
views. Details include the API's summary, description, icon, screenshots, and
permission metadata when Flathub provides it. Install and uninstall operations
use only the existing bridge target `flatpak`, with `install` / `uninstall`
calls taking `{ "ref": "app/ID/x86_64/stable" }` and progress delivered by the
`progress` topic. Installation/removal bars appear in app rows, details, and
the persistent header. They show reported percentages or animate while a stage
has no percentage. The UI does not call the Flatpak executable or linuxd.

Flathub's `/appstream/{id}` response does not include permission metadata for
every app. When `metadata.permissions` is absent, the detail page says the API
did not provide those details. Search uses the public `/search` POST endpoint;
popular and recently added use `/collection/popular` and
`/collection/recently-added`.

The per-app development signing key follows `tools/build-apps.sh`'s convention
at `~/.config/matonos-keys/matonos-flathub.jks`. `dev.sh` builds only this app,
installs its signed APK on port 5562, and launches it. Run `npm run typecheck`
and `npm run lint` from this directory for source checks.

## Verification

- `npm run typecheck`, `npm run lint`, and `node --check` for the config and
  release-linkage scripts pass.
- Preflight passes. Expo release packaging and the release-linkage check
  succeeded; `apps-built/Flathub.apk` is staged, its permission XML is present,
  and its signing certificate is registered.
- The 2026-09-29 23:27 full image includes Flathub at
  `/system_ext/priv-app/MatonOSFlathub/MatonOSFlathub.apk`. The coordinator also
  added its permission XML and `flatpak` caller authorization.
- In QEMU, Popular, New, search, and Installed screens were captured under
  `/mnt/data/aosp/out/pc-logs/flatpak-store/`. Opening a detail page with
  screenshots crashed with a nested vertical-scroll Compose exception. The
  source now uses a horizontal `LazyRow` for screenshots and paints a dark root
  background to fix the low text contrast. TypeScript and ESLint pass; this fix
  still needs an APK/image rebuild and fresh-VM verification.
- The Installed screen received the bridge's generic `The Flatpak operation
  failed.` response. During the Calculator install attempt, the bridge
  authorized `add_flathub`, but the detail-screen crash happened before the
  `install` call, so no download or progress event was observed. See
  `/mnt/data/aosp/out/pc-logs/flatpak-store/runtime-findings.txt`.

## Real PC checks

1. Boot a fresh MatonOS image on the Ryzen 5800X/RX 6600/Intel 7265 PC, Surface
   Pro 3, or HP ProDesk 600 G1. Confirm the app opens and renders Flathub's
   Popular and New collections and search results.
2. Open several details pages. Check summary, description, icon, screenshots,
   and whether the API provides permission metadata for each app.
3. Install a small test app. Confirm the progress text updates, completion is
   reported, and the app appears in Installed. Uninstall it and confirm it is
   removed from Installed.
4. Disconnect the network and stop linuxd in a test image separately. Browse
   and bridge failures should show a readable error without crashing the app.
