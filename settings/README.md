# MatonOS Settings

MatonOS Settings is the v2 system app in `rn-apps/settings`, built as an Expo SDK
57 / React Native 0.86 CNG project with Hermes and Expo UI Jetpack Compose
(Material 3). It uses package `org.matonos.settings` and the existing
`matonos-settings` development key. The previous Java stub remains parked
until a coordinated build imports the Expo APK under the existing Soong module
name `MatonOSSettings`.

## Entry points

The main activity advertises `com.android.settings.action.IA_SETTINGS` and
`com.android.settings.category.ia.homepage`, with title, summary, icon and
order metadata for AOSP Settings' homepage tile. It has no LAUNCHER filter.
A disabled `InstallAlias` activity-alias supplies the separate app-drawer
entry. The app's BOOT_COMPLETED receiver enables it only when
`ro.boot.matonos.live=1`; property read failures leave the alias disabled.

## Sections

The app uses Expo Router section routes and a Material 3 navigation rail.
Material Symbols from `expo-symbols` are rasterized to Compose image sources so
they remain children of the Jetpack Compose host. Every Compose button
includes an explicit Compose `Text` child and contrasting Material color
tokens. Section content uses one consistent 820 dp column; cards fill that
column and pad their contents, with labels and values laid out in Material
list-item slots. Sleep timeout uses a labelled slider and switch; sleep-mode
choices use a radio list. The top app bar returns through route history and
finishes the activity at the Settings root.

`expo-localization` selects the current locale for number formatting and the
string table. English is the shipped default and fallback; no translated copy
has been added yet. Disk capacity is shown in localized GiB.
The root system background and Compose color scheme follow Android's light or
dark setting; system-bar text uses Expo's automatic contrast style.

- **Install MatonOS** appears only when the live marker is set. The app requires an executor readiness response, successful dry-run and
target-disk readback flags before it lists targets or enables installation.
The app owns the v1 operation request model and layout plan copied from the former
`rn-apps/installer` project. It presents a separate-drive warning, lists drives
through the `install` channel, and submits one typed primitive at a time.
- **Sleep** reads `get_state` from the existing `sleep` channel and presents
idle-timeout and mode controls. The current daemon implements only
`get_state`; the setter requests are recorded for integration and report an
unavailable command until then.
- **Hardware** displays the absorbed Hardware API's battery and camera status.
Wi-Fi, Bluetooth, audio and GPU are explicitly Unknown until the bridge adds a
typed hardware summary; the app does not use hidden SystemProperties APIs.
- **Developer** opens the bridge's existing TrustedAppsActivity by its public
intent action.
- **About** shows Android build/device fields and identifies MatonOS as based
on AOSP.

`rn-apps/settings/modules/matonos-settings` contains the locally absorbed Expo
native module sources: MatonOS bridge client adapter, SystemIcon/Symbol and
Hardware API. The React Native Paper components and theme were not copied.

## Build and development

After the coordinating build is no longer RUNNING, the app can be built with
`device/maton/pc_x86_64/tools/build-apps.sh`; the registry keeps the app id
`matonos-settings`, fixed import name `MatonOSSettings`, staged APK name
`Settings.apk` and existing signing key. For development, start the agent VM
on adb port 5564 and run `rn-apps/settings/dev.sh`. It rebuilds the debug client,
then starts Metro with cache clear on port 8084; QEMU clients use host
`10.0.2.2:8084`.

## Verification status and open work

The Settings Expo release APK built and passed TypeScript, ESLint, Compose-only
release-linkage checks, signing and device-tree preflight. A full image rebuild
is requested. Fresh-boot screenshots of every section, the Settings homepage
tile, and the live-only launcher alias are pending that image. Do not use
destructive install operations: the install executor is not integrated and no
QEMU target-disk dry run has passed.

Required integration is listed in `SHARED-CHANGES.md`. A/B conversion remains
out of scope: live packaging still provides only slot A, installed images are
not A/B, boot-control integration is absent, the shared XBOOTLDR/slot-entry
contract is unproven, and encrypted user-file staging/restoration plus
installed-image installer dormancy still need implementation. The install
service must independently enforce live-only calls, fresh disk enumeration,
live-source exclusion, mount/in-use/read-only checks, bounds/path validation
and fail-closed behavior before any write primitive is enabled.

## Real hardware checks

1. Build and boot a fresh image on the Ryzen/RX 6600/Intel 7265 test PC.
2. Open Android Settings and confirm the MatonOS tile appears on the homepage;
   open it and check all five navigation sections.
3. Confirm MatonOS Settings has no ordinary app-drawer icon. Boot the live
   image and confirm the separate Install MatonOS icon appears. Boot an
   installed image and confirm that alias remains disabled.
4. Check battery, Wi-Fi, Bluetooth, audio, GPU and camera status against the
   detected hardware. Repeat with adapters unplugged and with no battery or
   camera present.
5. Check Sleep status with the service active and unavailable; until setter
   support lands, confirm unavailable controls do not claim that a setting
   was applied. Open Trusted developer apps and return to Settings.
6. Installer destructive testing is pending service integration. Use only a
   disposable QEMU target disk after the service's dry-run and readback
   verification have passed.
7. Repeat entry-point and hardware fallback checks on the Surface Pro 3 and
   HP ProDesk 600 G1.

## Bring your own GApps

The **Bring your own GApps** page starts with the fixed red risk warning and requires acknowledgement before it enables file selection or update removal. The page accepts user-selected APKs or zip archives, extracts flat and nested APK layouts plus LiteGapps `files/files.tar.xz`, checks each supported package for x86_64 native ABI, a version newer than the preinstalled stand-in, and Google's pinned signing lineage, then installs it through privileged PackageInstaller sessions. It also displays the Android ID used for uncertified-device registration. MatonOS does not ship, link to, host, or download Google APKs and does not claim Play certification or Play Integrity support.

The current GMS image includes stand-ins for Play services and Play Store. It does not include a GsfProxy package, so a Google Services Framework APK cannot update a stand-in until the GMS integration restores that privileged package. This is recorded in `SHARED-CHANGES.md`; the approved `patches/frameworks/base/0003-pinned-gms-update.patch` already contains the GsfProxy-to-GSF signer transition.

### Verification and hardware checks

- Java installer/picker sources compile with `javac` against the Android 37 SDK jar and the XZ Java API; XML manifests and app config parse. TypeScript and ESLint checks require the worktree's missing `node_modules` and remain pending.
- After the coordinator builds and boots a fresh image, acknowledge the warning, select a user-supplied x86_64 MindTheGapps zip, and confirm the UI reports separate ABI, version, and signing-lineage failures when given corresponding invalid packages. Confirm valid APKs install as updates and Google packages appear in Package Manager.
- Confirm the device ID is visible, then use PackageInstaller's update removal action and verify the microG stand-ins are restored. Repeat on the Ryzen/RX 6600/Intel 7265 PC, Surface Pro 3, and HP ProDesk 600 G1; also verify the page is usable without GSF and the error explains its missing stand-in.
