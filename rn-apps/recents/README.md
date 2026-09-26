# MatonOS Recents

MatonOS Recents (`org.matonos.recents`) is an independent Expo SDK 57 / React Native 0.86.3 CNG app. It owns the recent-task screen and an exported `RecentsComponentAnchor` activity alias for the non-empty SystemUI recents component setting. `RecentsActivity` extends `AppCompatActivity`, as required by Expo's host lifecycle. Its React screen lists current-user tasks through `apps/rn-common`, with switch, close and thumbnail actions delegated to the system bridge.

The app shares the pinned React, RN, Hermes, MD3 theme, UI components and typed MatonOS wrapper with `../launchme` and `../shelf`. Native code belongs under `modules/matonos-recents`; the generated `android/` project is ignored and disposable. The Recents APK needs its own persistent image signing key, privileged allowlist entry and bridge caller authorization; integration details are in `../launchme/SHARED-CHANGES.md`.

## Development

Run `./dev.sh` to build and install the dev client, start Metro on port 8083, reverse the port over adb and open Recents. `./dev.sh --reset` restores the image APK. Production builds embed Hermes bytecode and contain x86_64 native libraries only. Do not add `INJECT_EVENTS`; scoped navigation belongs to the platform bridge and is authorized only for Shelf.

## Checks

Run `npm ci`, `npm run typecheck`, `npm run lint`, `npm run format:check`, `CI=1 npx expo prebuild --platform android --clean --no-install`, then `./android/gradlew -p android --no-daemon --max-workers 2 :app:assembleRelease` and `npm run check:release-linkage`. The linkage check confirms SafeArea registration, the Recents Expo module, AppCompat, Hermes packaging and x86_64-only ABI. On a fresh image, `apps/launchme/scripts/smoke-split.sh` captures Home, Drawer, Recents and Settings with the Shelf visible and checks all three crash buffers/process memory reports.

The fresh-image task switch/close flows, SystemUI launch through the configured anchor, screenshot quality, APK size and combined Shell+Shelf+Recents PSS remain pending the coordinator image build.
