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

Capabilities (the store shows a feature only for sources that declare it):
- `HAS_REPOSITORIES`: user-managed repositories; `repos/addRepo/delRepo`.
- `HAS_AGE_RATINGS`: details carry `ageRating`.
- `HAS_REVIEWS`: `reviews(id)` returns reviews (others are never asked).
- `HAS_ANTI_FEATURES`: details carry `antiFeatures`.
- `HAS_PERMISSIONS`: details carry `permissions` / `safety`.
- `HAS_LOGIN`: the source supports signing in. Reserved; this version does
  nothing with it.
- `REQUIRES_SETUP`: the user must configure the source before it is useful.
  Reserved for a future version.

Flatpak: REPOSITORIES, AGE_RATINGS, REVIEWS, PERMISSIONS. F-Droid:
REPOSITORIES, ANTI_FEATURES, PERMISSIONS.

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

`install(id, operationId, callback)` is used for both and returns at once; the
work runs in the background. The source downloads and verifies, reports
`progress`, and hands every package to the store with
`commit(operationId, packageName, inherit, names, files, removeSplits, result)`:
one PackageInstaller session per call, `names[i]` the APK name of read-only
`files[i]` (base.apk plus split APKs), `inherit` keeps the installed package's
other splits, `removeSplits` drops splits. The session is pinned to
`packageName` and requests update ownership. The store answers on
`result.result(ok, error)` once the session is final; the source commits a
dependent package only after its dependencies succeeded. When everything is
in, the source calls `done(operationId)`, or `failed(operationId, error)`.
Sources never install or uninstall; the store uninstalls by package name.
