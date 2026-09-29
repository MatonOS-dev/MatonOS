# Flathub Store

An Expo app for browsing Flathub's public v2 API and installing system Flatpaks
through MatonOS System Bridge. The UI uses Expo UI's Jetpack Compose primitives.
It has Browse (popular and recently added), search, app details, and Installed
views. Details include the API's summary, description, icon, screenshots, and
permission metadata when Flathub provides it. Install and uninstall operations
use only the existing bridge target `flatpak`, with `install` / `uninstall`
calls taking `{ "ref": "app/ID/x86_64/stable" }` and progress delivered by the
`progress` topic. The UI does not call the Flatpak executable or linuxd.

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
  succeeded; `app-release.apk` was produced at about 66 MB.
- The full image build stopped on duplicate `uses_libs` and
  `optional_uses_libs` properties in shared `buildinfra/Android.bp` and
  `gms/Android.bp`. A later module build passed Soong analysis but stopped at
  `MatonAuroraPlaceholder` dexpreopt because that placeholder APK has no
  `classes.dex`. Neither failure is in this app.
- QEMU testing and requested screenshots are pending a successful image build.
  The coordinator's build status explicitly says no VMs until an OK image.
- Image import and bridge use still require the shared buildinfra and
  `flatpak` target allowlist additions described in `SHARED-CHANGES.md`.

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
