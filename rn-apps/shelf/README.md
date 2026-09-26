# MatonOS Shelf

MatonOS Shelf is a separate Expo SDK 57 / React Native 0.86.3 app (`org.matonos.shelf`). It owns the always-on-top overlay service and window, boot startup, compact and expanded sizing, the RN shelf surface, and the Java `ShelfView` fallback. It shares the pinned React / RN / Hermes stack, dynamic MD3 theme, components, and typed MatonOS bridge wrapper from `../rn-common`. The generated `android/` directory is disposable CNG output; make native changes in `modules/` or `plugins/`.

The overlay uses a Java service because Android requires a native owner for a persistent window. `ShellReactSurface` mounts the `MatonShelf` RN component in that window. If the service cannot start the RN surface, it swaps to Java buttons. At boot, `BootReceiver` starts the service; Shell sends explicit visibility broadcasts so the shelf expands on Home and stays as a slim bar over apps. Shelf actions use the shared MatonOS client for scoped Back, Home, Recents and recent-task operations. Apps opens Shell's `DrawerActivity`; Recents opens `org.matonos.recents/.RecentsActivity`; long-pressing Apps returns Home. The optional 3-button mode keeps live task icon buttons alongside Back/Home/Recents in their classic edge positions.

## Develop both apps

The apps use independent Metro servers and app-specific development keys. Start each in its own terminal:

```bash
cd ../launchme && ./dev.sh       # Shell Metro: 8081
cd ../shelf && ./dev.sh          # Shelf Metro: 8082
```

Each script builds and installs a debug Expo dev client signed by that app's own image key. Fast Refresh in Shell updates Home and Drawer; Shelf updates its overlay surface; Recents updates the task screen. `dev.sh --reset` removes only that app's system-app update and restores its image APK. The scripts target the windowed user VM at `127.0.0.1:5555`; configure `MATON_ADB_DEVICE` to select another VM. Each app owns a Metro port and adb reverse mapping, so they can run together.

Edit the shelf UI in `src/components/Shelf.tsx`; native Android service and fallback code lives under `modules/matonos-shelf/android`. Shared theme and MatonOS TS APIs live in `../rn-common`. Use strict TypeScript and avoid runtime code generation so release Hermes bytecode remains compatible with Static Hermes constraints.

Image builds use `tools/build-apps.sh` registry task `expo`, stage `MatonOSShelf.apk`, and sign with the distinct `matonos-shelf` key. The release is Hermes bytecode with x86_64 only and no Metro dependency. Run `npm run check:release-linkage` after assembly. Image verification must include the shelf over Settings, button actions, the Java fallback path, crash buffer, two APK sizes, and combined PSS for `org.matonos.shell` and `org.matonos.shelf`; those measurements are pending the coordinator's next image.
