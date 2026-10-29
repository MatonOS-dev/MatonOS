> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Software Center — F-Droid phases 1–3

The app has a shared TypeScript `Source` contract for search, details,
installed apps, updates, install, uninstall, and progress. `FlathubSource`
wraps the existing Flathub API and Flatpak bridge operations. `FdroidSource`
reads the built-in official F-Droid repository catalogue. Search merges both
catalogues into one list with a visible source badge. F-Droid apps can be
installed and removed through the Software Center. Updates appear in the
shared Updates view only when the installed version is older and its current
signing certificate set exactly matches the signer SHA-256 set in the index.
Missing signer data and signer mismatches are excluded.

The native Java index client downloads `entry.jar`, verifies its JAR signature
and signer fingerprint, reads `entry.json`, then verifies the SHA-256 of
`index-v2.json` before parsing or caching it under the app's private files
directory, separated by repository URL. The signer fingerprint is pinned from
[F-Droid's release signing keys documentation](https://f-droid.org/docs/Release_Channels_and_Signing_Keys/).
The catalogue parser and client are original code based on F-Droid's published
index format; no F-Droid client code is included.

Installing downloads every APK file listed for the selected version into the
app's cache, verifies each SHA-256 from the signed index, and only then creates
a PackageInstaller session. The session uses `base.apk` and split names read
from each APK manifest, requests update ownership on supported platform
versions, and reports PackageInstaller's result through the shared progress
event. Uninstall also uses PackageInstaller. Installed F-Droid catalogue rows
are reconciled with installed package names from PackageManager and appear in
the same Installed view as Flatpak apps.

WorkManager checks the official repository every 12 hours when a network is
available and records the update package IDs and check time. Opening the
Updates view performs a fresh verified-index check. Updating reuses the
PackageInstaller path and shared progress events; installs request update
ownership on platform versions that support it so later updates can be
prompt-free.

## Verification

- `modules/matonos-flathub/android/test-host.sh` runs host-Java checks for a
  valid signed JAR, a damaged signature, a wrong signer pin, an index hash
  mismatch, parser output from a fixture, and a wrong APK hash that must stop
  before the PackageInstaller commit callback. It also verifies that a higher
  version with a matching signer is offered and signer mismatch or a
  non-higher version is not.
- `npm run typecheck` and `npm run lint` check the TypeScript app.
- `MATON_AOSP_ROOT=/mnt/data/aosp MATON_APPS_ONLY=matonos-flathub tools/build-apps.sh`
  builds and signs the Expo app, then checks release linkage.
- `MATON_AOSP_ROOT=/mnt/data/aosp MATON_APPS_ONLY=matonos-flathub tools/build-apps.sh`
  completed successfully for phase 3. No image or VM was used.
- A fresh image/VM was not used; use the real-PC checks below for runtime
  validation.

## Phase 2 real PC checks

1. Install the rebuilt app on a fresh MatonOS image and confirm that the
   Software Center opens, searches both sources, and labels F-Droid results.
2. On a network-connected PC, install a small F-Droid app. Check download
   progress, PackageInstaller completion, and the merged Installed list; then
   uninstall it and confirm it disappears from that list.
3. Repeat with an index version that lists multiple APK files and verify the
   base APK and splits install in one PackageInstaller session.
4. Check reconciliation for a package already installed on the PC and confirm
   an installed package absent from the F-Droid catalogue is not shown as an
   F-Droid app.
5. Install an older F-Droid package version, then verify it appears in Updates
   only when its signing certificate matches the index signer. Apply the
   update and check the shared progress bar and installed version.
6. Leave the device network-connected for a WorkManager run and verify the
   check time and update package list survive closing and reopening the app.
7. Repeat on the Ryzen 5800X/RX 6600/Intel 7265 PC, Surface Pro 3, or HP
   ProDesk 600 G1. F-Droid install behavior should not depend on Wi-Fi,
   Bluetooth, or audio hardware availability.

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

## Historical Flathub real PC checks

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
