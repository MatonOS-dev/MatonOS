# `@matonos/rn-common`

Shared UI and typed MatonOS client for MatonOS apps. Current apps pin Expo SDK 57, React 19.2.3, React Native 0.86.3, React Native Paper 5.15.3, and Material Color Utilities 0.4.0. Keep those versions aligned in all consumers.

## Public API

- `matonTheme.ts` creates Material 3 light/dark themes seeded from wallpaper colors, with system theme selection.
- `MatonButton` is the shared Paper-based button used by app surfaces.
- `MatonOS.ts` and `useMatonOS` expose typed promise calls, startup/availability state, topic event subscriptions, and wallpaper color.
- `useBridgeAvailable` listens for bridge availability changes.
- `native-specs/NativeMatonOS.ts` is the Codegen TurboModule contract. Its Android implementation wraps the shared `buildinfra/client` Java client; do not reimplement or fork that client here.

## Reuse from another app

Declare `@matonos/rn-common` as a local package dependency and include its Android Gradle project so Codegen and autolinking build `MatonOSClient`. Point TypeScript and Metro at the package source, then run the app's locked npm install before Gradle. The release build must bundle Hermes bytecode; Metro is for debug development only. Keep package versions aligned across consumers and document any shared dependency changes before changing the pinned set.
