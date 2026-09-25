# Shared changes required for the Expo CNG shell

MatonOSShell is a standard Expo SDK 57 / React Native 0.86.3 project using Continuous Native Generation. Its former `Android.bp` has been removed. Please integrate these shared changes before the next image build:

1. In `device/maton/pc_x86_64/buildinfra/apps/apps.list`, register:
   `matonos-shell<TAB>../../../apps/launchme<TAB>assembleRelease<TAB>MatonOSShell.apk` (project path is relative to the device-tree root)
2. In `device/maton/pc_x86_64/buildinfra/Android.bp`, add the fixed `android_app_import` named `MatonOSShell` for `apps-built/MatonOSShell.apk`, with `system_ext_specific: true`, `privileged: true`, `certificate: "PRESIGNED"`, `overrides: ["Home", "Launcher2", "Launcher3", "Launcher3QuickStep"]`, and a required `prebuilt_etc` module for `privapp-permissions-org.matonos.shell.xml`.
3. Add that permissions XML as an imported file under `/system_ext/etc/permissions`. It grants only `org.matonos.permission.SYSTEM_BRIDGE` to `org.matonos.shell`.
4. Add an exact `launcher org.matonos.shell` line to `systembridge/res/raw/target_caller_allowlist.txt`; `tools/build-apps.sh` will generate the shell signing certificate digest in `caller_cert_allowlist.txt` from the app-specific development key.
5. Extend `systembridge/aidl/org/matonos/systembridge/ISystemBridge.aidl` and implement the methods below in the platform-signed bridge. These are the only privileged shell operations; authorize each against the `launcher` target and the exact caller package/certificate.

```aidl
boolean ensureShellOverlayAccess();
String getRecentTasks(int maxTasks); // JSON [{"taskId":int,"packageName":string,"windowingMode":int}], current user, MRU first
boolean moveTaskToFront(int taskId); // only a task visible in getRecentTasks
boolean setTaskFullscreen(int taskId); // only a task visible in getRecentTasks; use WM Shell/task APIs
```

`ensureShellOverlayAccess()` should grant the verified shell package the `SYSTEM_ALERT_WINDOW` app-op needed by the public `TYPE_APPLICATION_OVERLAY` shelf. The existing `injectBackKey()` bridge method is used for the Back button; add `input org.matonos.shell` to the exact target allowlist as its current authorization target. Please bump the bridge API version/hash and update the Gradle client AIDL output by rebuilding the app.

The app currently binds to System Bridge using its explicit component and the existing `SYSTEM_BRIDGE` permission. No private framework stubs are used by the app.

## Device configuration and retired patches

The launcher-owned reversals for frameworks/base 0001 and 0004 and Launcher3 0001–0003 have been applied to the AOSP worktrees, and those five device patch files have been removed. Please remove the stale device properties `ro.matonos.always_taskbar`, `ro.matonos.shelf_nav`, and `ro.matonos.maximize_fullscreen` from `device.mk` (they were only consumed by the retired patches), and remove the corresponding maximize property type/context if no remaining consumer exists.

The overlay still sets `config_recentsComponentName` to the shell activity, but the Gradle shell intentionally no longer implements the private Quickstep service. Please choose a stock, no-patch navigation/taskbar configuration that avoids SystemUI attempting to bind a nonexistent Quickstep endpoint or drawing a second shelf. Keep the desktop-mode capability booleans and the independent keyboard+mouse patch unchanged for now.

`device.mk` currently keeps `MatonOSShell` in `PRODUCT_PACKAGES` and filters out `Launcher3QuickStep`. The module name remains `MatonOSShell` after the fixed prebuilt import is added, so keep the package entry; once the imported APK's `overrides` are in place, remove the `Launcher3QuickStep` filter line and tell me.

## Recents close action required

The app needs a narrowly authorized operation for the user-requested close button in Recents. Please extend `ISystemBridge.aidl` with:

```aidl
boolean removeRecentTask(int taskId);
```

The implementation must authorize target `launcher` for the exact caller, confirm that `taskId` is still in `getRecentTasks()` for the calling user's current profile, refuse MatonOS Shell/SystemUI tasks, and then remove the task through the framework task API. Bump the bridge API/hash and regenerate the Gradle client's AIDL output. Until this lands, the launcher cannot safely close another app's task using public APIs.

## Keep the shelf visible over Settings' overlay protection

AOSP SettingsBaseActivity / SettingsHomepageActivity enable `HIDE_NON_SYSTEM_OVERLAY_WINDOWS`; WM hides ordinary `TYPE_APPLICATION_OVERLAY` windows even when their app-op and service are healthy. A MatonOS app configured as the recents package receives `SYSTEM_APPLICATION_OVERLAY` through the platform's `recents` permission flag, but only the privileged bridge can set the corresponding `LayoutParams` system-overlay marker without exposing hidden APIs in the launcher. Please add:

```aidl
Bundle prepareShellOverlay(in Bundle request); // request/result key: "windowParams" (WindowManager.LayoutParams)
```

Authorize only `launcher` / `org.matonos.shell`; require `TYPE_APPLICATION_OVERLAY`, validate bounded width/height and flags, require the shell package to hold `SYSTEM_APPLICATION_OVERLAY`, then call `LayoutParams.setSystemApplicationOverlay(true)` and return the adjusted params in the response bundle. Deny all other window types and callers. Bump the bridge API version/hash and regenerate the Gradle client. The launcher will use this before `WindowManager.addView`; it retains a normal-overlay fallback for images with an older bridge.

## Fresh-image findings (2026-09-25, image 15:58)

- `getRecentTasks(32)` returns an empty result to the launcher even while the framework's `dumpsys activity recents` reports a live, visible app task. Reproduced with Fossify Math: task id 15 was `hasTask=true` and its `baseActivity` was `org.fossify.math/.activities.MainActivity`, while RecentsActivity displayed “No recent apps.” Please diagnose the bridge's recent-task retrieval/filtering and return these current-user app tasks in MRU order. The launcher already filters Shell/SystemUI and restricts display to launchable packages.
- `removeRecentTask(int)` and `prepareShellOverlay(Bundle)` were not present in the System Bridge API in this image. Please land these along with the recent-task fix, regenerate the Gradle client, and request a new full image. Settings screenshots confirm its `HIDE_NON_SYSTEM_OVERLAY_WINDOWS` behavior hides the shelf even though the `MatonOS shelf` overlay window and service remain present.

## Expo CNG build helper

For the `matonos-shell` registry row, extend `tools/build-apps.sh` to:

1. Run `npm ci --ignore-scripts` in `apps/launchme`.
2. Run `CI=1 npx expo prebuild --platform android --clean --no-install`; generated `android/` must stay ignored/uncommitted.
3. Invoke the generated Android project at `android/` using its Gradle wrapper (`./android/gradlew -p android :app:assembleRelease`, SDK 57 currently pins Gradle 9.3.1 / AGP 8.12.0). The fixed project registry Gradle 9.4.1 / AGP 9.2.0 does not compile the Expo 57 RN settings plugin (Kotlin metadata mismatch).
4. Supply `MATON_SIGNING_STORE_FILE`, `MATON_SIGNING_STORE_PASSWORD`, `MATON_SIGNING_KEY_ALIAS`, and `MATON_SIGNING_KEY_PASSWORD` before prebuild/build. The CNG plugin signs debug and release with the existing 07522fac… shell certificate; do not generate or rotate the launcher key.
5. Stage `android/app/build/outputs/apk/release/app-release.apk` as `MatonOSShell.apk` in both existing staging locations, and stage the plugin-generated `android/app/src/main/assets/privapp-permissions-org.matonos.shell.xml` to `buildinfra/privapp-permissions/privapp-permissions-org.matonos.shell.xml` (or preserve the identical checked-in source file).

Expo 57's generated project requests compile/target SDK 36 and build tools 36.0.0. Ensure the SDK installer provisions `platforms;android-36` as well as the existing platform/build-tools packages. The first Gradle wrapper download and `npm ci` may use the network; subsequent builds should use the pinned local npm cache and Gradle distribution/cache. Keep release bundling enabled: Expo CLI embeds the JS and Hermes compiles it into bytecode; Metro must not be required by production APKs.

### Build result from the launcher row

I verified the generated wrapper with Expo SDK 57 / RN 0.86.3: `:app:assembleRelease` succeeds using Gradle 9.3.1 / AGP 8.12.0, and the resulting APK is 60,549,570 bytes, signed with the existing shell certificate, x86_64-only, with the production Hermes bundle embedded. The generic helper's Gradle 9.4.1 / AGP 9.2.0 cannot build this generated project because of the RN settings plugin Kotlin metadata mismatch. Please stage the output and the generated permission XML into the image. The launcher full-image request is appended to `out/pc-logs/agents/build-requests.txt`.

### Safe-area release crash-loop fix and smoke test

The RN APK in the image from 18:31 crashed because `RNCSafeAreaProvider` was absent. The old `legacy-gradle/react-native.config.js` disabled Android autolinking for `react-native-safe-area-context`; it is no longer disabled, and the dependency is now pinned directly in the app. A clean Expo prebuild generates a `PackageList.java` entry for `SafeAreaContextPackage`; the signed release DEX includes that package and `RNCSafeAreaProvider`, and `lib/x86_64/libreact_codegen_safeareacontext.so` is present. `npm run check:release-linkage` verifies these after release assembly. Current corrected output is `android/app/build/outputs/apk/release/app-release.apk` (60,565,954 bytes, existing 07522fac… signer).

The native module also now renders a Java Home app grid if the React Home surface or host lifecycle setup fails; the service retains and uses its Java shelf fallback on RN surface startup failure. After staging/building the fresh image, run `apps/launchme/scripts/smoke-image.sh <adb-serial> out/pc-logs/launcher/<run-name>`; it starts Home, checks the activity and crash buffer, and saves `home.png` plus logs. Please review the screenshot before marking the boot check passed. Device verification remains pending because adb 5555 was offline at this handoff.
