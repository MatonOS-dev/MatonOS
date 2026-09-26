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

`ensureShellOverlayAccess()` should grant the verified shelf package the `SYSTEM_ALERT_WINDOW` app-op needed by the public `TYPE_APPLICATION_OVERLAY` window. The old `injectBackKey()`/`input` authorization is retired for navigation; use the narrow audited `nav.*` methods in the final section below. Please bump the bridge API version/hash and regenerate the Gradle client AIDL output.

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

## Separate MatonOS Shelf app (2026-09-25)

Please integrate the new Expo CNG project `apps/shelf` as a separate privileged prebuilt:

1. Add `matonos-shelf<TAB>../../../apps/shelf<TAB>expo<TAB>MatonOSShelf.apk` to `buildinfra/apps/apps.list`. `tools/build-apps.sh` must create and persist a distinct `matonos-shelf` signing key (do not reuse the Shell key), supply it to CNG through the `MATON_SIGNING_*` variables, and stage `apps/shelf/android/app/src/main/assets/privapp-permissions-org.matonos.shelf.xml` beside the APK. The project Gradle alias `expo` depends on `assembleRelease`.
2. Import the APK as `MatonOSShelf` under `/system_ext/priv-app`, `system_ext_specific: true`, `privileged: true`, `certificate: "PRESIGNED"`, and include a `prebuilt_etc` requirement for its permissions XML. Add `MatonOSShelf` to `PRODUCT_PACKAGES` alongside `MatonOSShell`; leave the existing shell/filter lines until this import is active. The XML grants only `org.matonos.permission.SYSTEM_BRIDGE` to `org.matonos.shelf`.
3. Add exact target caller allowlist entry `launcher org.matonos.shelf`. Do not add `input org.matonos.shelf`; navigation must use the scoped `nav.*` operations requested below. Generate the matching `caller_cert_allowlist.txt` digest from the Shelf-specific signing key. Preserve the existing Shell target entries.
4. Make the Shelf app eligible to start its exported `BootReceiver` at boot (the receiver starts its non-exported `ShelfService`) and retain the manifest's explicit systembridge permission. Keep overlay permission ownership with Shelf; Shell no longer declares `SYSTEM_ALERT_WINDOW` or `RECEIVE_BOOT_COMPLETED`.
5. Update the launcher decision in `device/maton/pc_x86_64/NOTES.md` to: "MatonOS Shell and MatonOS Shelf are separate Expo SDK 57 CNG apps. Shell contains fullscreen Home, Drawer and Recents as separate registered React roots. Shelf owns the persistent overlay service/window, its RN surface and Java fallback, compact/expanded shelf UI and boot receiver. Both consume apps/rn-common for theme, shared components and the MatonOS client wrapper. They use independent app keys, Metro ports 8081/8082 and Hermes runtimes."
6. Ensure the app build SDK installer provisions `platforms;android-36` and `build-tools;36.0.0`; Expo SDK 57 generates compile/target SDK 36 projects. Keep the generated `android/` trees ignored and use each project’s pinned Gradle wrapper (9.3.1 / AGP 8.12.0).
7. The Shelf JS uses the typed `MatonOS` wrapper from `apps/rn-common` for recent tasks, switch/close/fullscreen and Back. Please add these methods to `MatonosClient` in `buildinfra/client`, returning its existing `MatonosClient.Result<Boolean>` and preserving bridge-unavailable reasons: `moveTaskToFront(int taskId)` and `setTaskFullscreen(int taskId)`. Both already exist in the systembridge AIDL and are authorized for the exact `launcher` caller. The JS TurboModule currently routes them through the client's bound bridge interface; moving them behind typed client methods keeps all bridge access in the shared client. Add `ensureShellOverlayAccess()` as `Result<Boolean>` too, so Shelf's native service can use the shared client for overlay setup. `prepareShellOverlay(WindowManager.LayoutParams)` already exists on MatonosClient. The apps must not call hidden framework APIs directly.

I will queue a full image build request after both generated Android projects pass local release assembly and linkage checks. Fresh-image VM screenshots, APK sizes and combined Shell+Shelf PSS remain required before this split is verified.

## Recents app, AppCompat lifecycle fix, and shelf navigation (2026-09-25)

This final section supersedes the earlier two-app decision text above: Recents is its own Expo app, and the overlay component must resolve to the Recents app's anchor.

The 22:31 image crash-looped because Expo's React lifecycle requires `AppCompatActivity`. Shell `HomeActivity` and `DrawerActivity` now extend `androidx.appcompat.app.AppCompatActivity`; Shelf hosts RN from a `Service` only; the new Recents `RecentsActivity` also extends `AppCompatActivity`. I added crash-buffer matching for `Current Activity is of incorrect class` and `AppContext.onHostResume` to `scripts/smoke-image.sh`, and expanded `scripts/smoke-split.sh` to boot Home, Drawer, Recents, Settings+Shelf, collect crash logs and all three process memory dumps.

Please integrate `apps/recents` as a third Expo SDK 57 / RN 0.86.3 CNG APK:

1. Add `matonos-recents<TAB>../../../apps/recents<TAB>expo<TAB>MatonOSRecents.apk` to `buildinfra/apps/apps.list`; create a distinct persistent signing key `matonos-recents` and generated cert digest. Add/import module `MatonOSRecents` in the prebuilt Android.bp and `PRODUCT_PACKAGES`, plus its privileged-permission XML (only `org.matonos.permission.SYSTEM_BRIDGE`). Stage the XML generated from `apps/recents/android/app/src/main/assets/privapp-permissions-org.matonos.recents.xml`.
2. Add exact caller allowlist entries `launcher org.matonos.recents` and `launcher org.matonos.shelf` for recent task listing, moving tasks, closing tasks and task thumbnails. Preserve any existing Shell entry. The Recents APK is x86_64-only and uses the same Expo/RN/Hermes versions as Shell and Shelf, but its own app key.
3. Keep `config_recentsComponentName` non-empty, and change it to `org.matonos.recents/.RecentsComponentAnchor`. The alias is exported and targets `org.matonos.recents.RecentsActivity`, which extends `AppCompatActivity`. It must not point at Shell. This avoids the SystemUI null-component failure while transferring Recents ownership to the Recents package.
4. Update `device/maton/pc_x86_64/NOTES.md` to record three separate updateable Expo apps: Shell owns Home+Drawer; Shelf owns the persistent overlay UI/window and its Java fallback; Recents owns the recent-task React screen and its task anchor. All three use `apps/rn-common` and pinned versions Expo 57 / RN 0.86.3 / React 19.2.3 / Hermes. Metro ports are 8081, 8082, 8083. Each app has a distinct key and Hermes runtime.
5. Update the build helper to provision `platforms;android-36`, run the Recents CNG prebuild with its Gradle wrapper, stage `MatonOSRecents.apk` and its permission XML, and generate the certificate digest before building the bridge. The Recents `dev.sh` uses port 8083 and its own development key.

### Scoped navigation and thumbnails in System Bridge

Please add these platform-bridge operations and typed wrappers to `buildinfra/client/MatonosClient`; apps call them only through the `apps/rn-common` wrapper. Audit every action and authorize exact package/target pairs. Bump the API version/hash and regenerate client AIDL:

```aidl
boolean navigateBack(boolean longPress); // target nav.back; only org.matonos.shelf
boolean navigateHome();                  // target nav.home; only org.matonos.shelf
boolean navigateRecents();               // target nav.recents; only org.matonos.shelf
byte[] getRecentTaskThumbnail(int taskId); // target launcher; Shell/Shelf/Recents exact callers
```

Add only `nav.back org.matonos.shelf`, `nav.home org.matonos.shelf`, and `nav.recents org.matonos.shelf` to the exact caller allowlist (plus their Shelf certificate digest). Do not authorize `input` for Shelf and do not grant `INJECT_EVENTS` to any app. The platform-signed bridge performs Back key down/up (hold for the `longPress` path so Android's normal back dispatcher/predictive-back path receives the event), starts the HOME intent for Home, and opens the configured Recents component for App Switch. If Recents is already foreground, a repeated/double Recents action returns to the previous current-user task, matching stock App Switch. The Recents thumbnail method must confirm the task is still a current-user app task, exclude Shell/SystemUI/Recents, bound output size, and return a PNG byte array; bridge task list/switch/close remains MRU and task-ID validated.

Please add `Result<Boolean> moveTaskToFront(int taskId)` and `Result<Boolean> setTaskFullscreen(int taskId)` to `MatonosClient` alongside the existing typed `getRecentTasks` and `removeRecentTask` methods. The `apps/rn-common` TurboModule delegates these calls through the typed client; its navigation and thumbnail adapters likewise invoke the corresponding client methods. Do not expose `bridgeInterface` to the app module for these operations. Return the client availability/reason unchanged when the bridge or capability is unavailable.

Stock keyboard mappings (Esc/Alt+Left, Meta/Home, Alt+Tab/App Switch) should remain in force; please verify them on the fresh image before adding any key interception. Shelf's default layout remains Back | Apps/tasks | Recents; its Shelf Settings menu toggles persistent 3-button mode (Back/Home/Recents with live task icons alongside). Home from an app returns to Shell Home; Home while Home is visible opens Drawer; Home while Drawer is open returns to Home. The same behavior applies to the shelf button and Apps long-press.

A full image request is queued after the three local APK builds. Fresh-image screenshots, bridge-audit evidence, crash-free boot, navigation semantics and total PSS across `org.matonos.shell`, `org.matonos.shelf`, and `org.matonos.recents` remain pending.

## Shelf bridge-thread and overlay follow-up (2026-09-26)

`ShelfService` now posts every bridge-availability notification to the main looper, and `installShelf()` defensively reposts itself if invoked off-main. This fixes the 10:06 boot crash where `matonos-bridge-check` called `WindowManager.addView()` directly. Shelf overlay setup now uses `MatonosClient.ensureShellOverlayAccess()` and `MatonosClient.prepareShellOverlay(LayoutParams)`; it no longer invokes those overlay Binder methods reflectively from `ShelfService`.

The 10:06 image still cannot satisfy the system-overlay gate for the Shelf-owned window: `prepareShellOverlay()` requires `SYSTEM_APPLICATION_OVERLAY`, but the platform currently assigns that permission only to the configured Recents role (`org.matonos.recents`). `SYSTEM_ALERT_WINDOW` app-op is insufficient. Please grant `SYSTEM_APPLICATION_OVERLAY` to the exact allowlisted package `org.matonos.shelf` through the platform's controlled permission configuration (while retaining the bridge's exact permission and caller checks), or provide an equivalent narrowly scoped platform-owned authorization that allows only this Shelf overlay. Keep raw Binder access out of the app. Once included in a fresh image, verify Shelf remains visible over Settings with `HIDE_NON_SYSTEM_OVERLAY_WINDOWS` enabled.

## Shelf system-overlay permission and layout follow-up (2026-09-26)

Image 10:52 confirms the Shelf service is healthy, but Settings hides its `TYPE_APPLICATION_OVERLAY` window (`mIsForceHiddenNonSystemOverlayWindow=true`, `isVisible=false`) because `org.matonos.shelf` has only `SYSTEM_ALERT_WINDOW`. Shelf now requests `android.permission.SYSTEM_APPLICATION_OVERLAY` in its Expo app config and source manifest, and its generated `privapp-permissions-org.matonos.shelf.xml` grants that permission in addition to `org.matonos.permission.SYSTEM_BRIDGE`. Please ensure the updated generated XML is staged/imported on the same privileged partition as the Shelf APK (system_ext); keep this grant narrowly scoped to `org.matonos.shelf` and allowlist it in any platform permission policy needed for privileged grants. The existing `prepareShellOverlay()` bridge path already exact-checks Shelf's caller and permission, then calls `LayoutParams.setSystemApplicationOverlay(true)`; the app must continue to use that typed bridge call.

Shelf now follows `Settings.Secure.navigation_mode == 0` for its initial layout, showing Back/Home/Recents with running task buttons. A choice in Shelf Settings explicitly overrides that default and persists. The React Home no longer shows the stray Compose demo Switch under the app shortcuts; the Compose package remains available to the app.
