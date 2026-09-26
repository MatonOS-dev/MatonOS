# MatonOS launcher and shelf design

MatonOS Shell (`org.matonos.shell`), MatonOS Shelf (`org.matonos.shelf`) and MatonOS Recents (`org.matonos.recents`) are separate Expo SDK 57 CNG apps. They pin the same React Native 0.86.3, React 19.2.3, Hermes, Expo UI and Paper versions and consume shared theme, components and the MatonOS client wrapper from `apps/rn-common`.

Shell contains only fullscreen Home and App drawer. Native AppCompat activities select the separate roots `MatonHome` and `MatonDrawer`; there is no prop-switched multi-screen root. The AppCompat Recents activity and `MatonRecents` root live in the Recents APK; its exported `RecentsComponentAnchor` is the non-empty component configured for SystemUI. Shell retains a Java Home grid fallback if React initialization fails.

Shelf owns the always-on overlay service/window, boot receiver, compact/expanded state, Back/Apps/Recents and task buttons, a persistent 3-button preference, and Java fallback. Its own ReactHost mounts the shelf root into the service-owned overlay. Shell broadcasts whether Home is visible, so Shelf expands at Home and remains compact over apps. Back/Home/Recents navigation goes through narrowly authorized, audited systembridge methods. Recents uses the shared typed client wrapper for current-user tasks, switching, closing and thumbnails.

Expo CNG owns generated Android projects. Native services, activities, Expo modules and fallback views live in each app's `modules/`; config plugins generate manifest, ABI, bridge/permission and signing changes. Release APKs use Hermes bytecode, x86_64 only and separate app-specific signing keys. Each package has its own Hermes runtime, so record individual APK size and total Shell+Shelf+Recents PSS after image verification. Dev builds use independent Metro servers on 8081 (Shell), 8082 (Shelf) and 8083 (Recents).

Expo UI (Jetpack Compose) is the target UI kit. Paper remains transitional while screens migrate. Keep TypeScript strict and Static Hermes friendly, and use `rn-common` for shared UI and bridge contracts. The Settings app should consume this same package and pin the matching Expo/RN/Hermes versions.

## Validation

Local CNG, TypeScript, lint, release-linkage and Gradle results, fresh-image screenshots, APK sizes and combined PSS are recorded in `launcher.final.md` after the coordinator supplies an image. Remaining risks include Android's service-hosted React lifecycle and overlay policy over protected Settings windows; verify on a fresh boot and retain the Java shelf fallback.
