# Store source plugins

A store backend is an app with a bound service that:

- handles the intent action `org.matonos.store.SOURCE`,
- requires `android:permission="org.matonos.permission.BIND_STORE_SOURCE"`
  (signature permission defined by the store; services without it are ignored),
- implements `org.matonos.store.source.ISoftwareSource`
  (`modules/matonos-flathub/android/src/main/aidl/org/matonos/store/source/`;
  copy both AIDL files unchanged, method order is the transaction order).

Preinstalled (system) plugins are enabled by default. Any other plugin stays
off until the user enables it in the store.

## Data (contract version 1)

All calls return JSON strings. `info()` returns
`{"version":1,"name":..,"label":..,"capabilities":[..]}`; the store skips a
plugin whose version differs.

- `search`, `installed`, `updates`: arrays of AppListing
  (`id, name, summary, icon, developer, rating?, installed?, versionName?,
  origin?`, plus `packageName` when the Android package differs from `id`).
- `details`: one AppDetails (AppListing plus `description, screenshots,
  permissions, license?, homepage?, downloadSize?, installedSize?, safety?,
  antiFeatures?, ageRating?`). Types: `src/sources/Source.ts`.
- `reviews`: array of `{id, author, rating, summary, description, version,
  date}` (ISO date).
- `repos`: array of `{id, url, label, official, broken}`.

## Install and update

`install(id, operationId, callback)` is used for both. The plugin downloads
and verifies, reports `progress`, then calls `apks(operationId, packageName,
files)` with the base APK and every split APK of that one package as read-only
file descriptors. The store commits them in one PackageInstaller session
(split APKs supported; the session is pinned to `packageName`), and requests
update ownership. On error the plugin calls `failed`. Plugins never install
or uninstall; the store uninstalls by package name.
