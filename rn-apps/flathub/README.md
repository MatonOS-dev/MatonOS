# Software Center

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
  succeeded; `apps-built/Flathub.apk` is staged, its permission XML is present,
  and its signing certificate is registered.
- The 2026-09-29 22:00 full image build completed successfully, but the image
  has no Flathub APK. `buildinfra/Android.bp` and `buildinfra/buildinfra.mk`
  do not import or package the app. The `flatpak` caller allowlist also still
  authorizes only Settings. See `SHARED-CHANGES.md` for the exact additions.
- QEMU screenshots were not captured because this OK image does not include
  Flathub; the user instruction is to report the missing buildinfra wiring
  instead of editing it.

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
