# MatonOS Shelf

MatonOS Shelf is a separate Expo SDK 57 / React Native 0.86.3 app (`org.matonos.shelf`). It provides the navigation UI, compact and expanded sizing, the RN surface, and Java `ShelfView` fallback. The platform-signed system bridge owns the protected bottom window, reserves navigation insets, and embeds the selected provider's `SurfacePackage` with `SurfaceControlViewHost`. Shelf never requests `SYSTEM_APPLICATION_OVERLAY` and does not own that protected window. It shares the pinned React / RN / Hermes stack, dynamic MD3 theme, components, and typed MatonOS client wrapper from `../rn-common`. The generated `android/` directory is disposable CNG output; make native changes in `modules/` or `plugins/`.

The bridge binds the provider service after a user explicitly selects it in MatonOS Settings. This binding starts Shelf at boot; Shelf's boot receiver no longer starts a background service. If the bridge cannot be reached, Shelf can use its regular `SYSTEM_ALERT_WINDOW` overlay where Android permits it, with Java controls as the RN failure fallback. Shell sends explicit visibility broadcasts so the shelf expands on Home and stays as a slim bar over apps. Shelf actions use the shared MatonOS client for scoped Back, Home, Recents and recent-task operations. Apps opens Shell's `DrawerActivity`; Recents opens `org.matonos.recents/.RecentsActivity`; long-pressing Apps returns Home. The optional 3-button mode keeps live task icon buttons alongside Back/Home/Recents in their classic edge positions.

The provider service implements the typed `NavigationBarProviderAdapter` from `buildinfra/client` and advertises action `org.matonos.systembridge.NAVIGATION_PROVIDER`. The adapter privately receives the host token and display id, creates the `SurfaceControlViewHost`, and asks Shelf only for a root `View`; apps do not handle raw Binder tokens. The bridge embeds the returned `SurfacePackage` in its own `SurfaceView`. The selected package's signing certificate is pinned until the user revokes it. Shelf is the preset provider, but the capability is disabled until explicit selection. MatonOS Settings can open the confirmation screen with action `org.matonos.systembridge.NAVIGATION_PROVIDER_SETTINGS`.

## Develop both apps

The apps use independent Metro servers and app-specific development keys. Start each in its own terminal:

```bash
cd ../launchme && ./dev.sh       # Shell Metro: 8081
cd ../shelf && ./dev.sh          # Shelf Metro: 8082
```

Each script builds and installs a debug Expo dev client signed by that app's own image key. Fast Refresh in Shell updates Home and Drawer; Shelf updates its overlay surface; Recents updates the task screen. `dev.sh --reset` removes only that app's system-app update and restores its image APK. The scripts target the windowed user VM at `127.0.0.1:5555`; configure `MATON_ADB_DEVICE` to select another VM. Each app owns a Metro port and adb reverse mapping, so they can run together.

Edit the shelf UI in `src/components/Shelf.tsx`; native Android service and fallback code lives under `modules/matonos-shelf/android`. Shared theme and MatonOS TS APIs live in `../rn-common`. Use strict TypeScript and avoid runtime code generation so release Hermes bytecode remains compatible with Static Hermes constraints.

Image builds use `tools/build-apps.sh` registry task `expo`, stage `MatonOSShelf.apk`, and sign with the distinct `matonos-shelf` key. The release is Hermes bytecode with x86_64 only and no Metro dependency. Run `npm run check:release-linkage` after assembly. Image verification must include the shelf over Settings, button actions, the Java fallback path, crash buffer, two APK sizes, and combined PSS for `org.matonos.shell` and `org.matonos.shelf`; those measurements are pending the coordinator's next image.
