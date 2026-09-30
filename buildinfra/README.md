# MatonOS out-of-Soong build infrastructure

MatonOS apps and native daemons are developed and compiled outside Soong.
Soong contains the fixed stable channel interface and privileged app imports
in `buildinfra/Android.bp`; native drivers and runtime assets are packed into the
ODM image by `tools/build-bundle.sh`. The platform-signed `systembridge/` app
is the only app source built by Soong because it is the narrow owner of hidden
framework APIs.

## Build tools and scripts

`tools/build-apps.sh` builds Gradle projects. The pilot is
`buildinfra/apps/MatonOSSettings/`, pinned to Gradle 9.4.1 and Android Gradle
Plugin 9.2.0. Gradle uses the checkout's AOSP API 35 and 36 public stub jars and a
user-local SDK at `~/Documents/matonos-android-sdk`. The script installs the
pinned SDK build tools 36.0.0 and downloads command-line tools revision
15859902 with a checked SHA-256. It takes Java 21 from the AOSP prebuilts when
no `JAVA_HOME` is set.

For system APIs, prefer a public API plus a method in System Bridge. A Gradle
app may use `compileOnly files(...)` for a framework stub/classes jar only as
a fallback. Set `MATON_FRAMEWORK_STUB_JAR` to the jar from the just-built AOSP
output (normally `out/target/product/pc_x86_64/system/framework/framework.jar`);
`build-apps.sh` can refresh the app's ignored `libs/framework-stubs.jar` copy
before each build. Rebuild the AOSP framework first whenever the platform API
changes. The jar is compile-only and is never packaged in an APK.

`buildinfra/apps/apps.list` is the app registry. It includes Settings, Shell,
Shelf and Recents image apps plus AudioToneTest, which is staged for `adb install`
testing only. Each app has its own development keystore under
`~/.config/matonos-keys/<app>.jks`; the matching password is stored in a
mode-0600 file beside it. Keys are generated on first use and never enter git.
The settings pilot's certificate SHA-256 is written to the fixed
`systembridge/res/raw/caller_cert_allowlist.txt` resource before AOSP compiles
the bridge. Shell, Shelf and Recents have distinct persistent app keys; the
Expo CNG projects use their pinned Gradle wrapper (Expo 57 uses Gradle 9.3.1 /
AGP 8.12.0), compile against API 36, and run `check:release-linkage` before
their APK and generated privapp XML are staged. Keep one key per package so F-Droid or other app updates cannot
inherit another app's bridge access. Release builds will use separate release
keys created and backed up offline, held outside the build host; the OS build
server receives a protected signing step or a pre-signed APK. Never promote a
development key to a release key, and do not rotate a release key without an
OS migration plan for app updates and bridge allowlists.

`tools/build-native.sh` uses NDK r30 at `~/Documents/android-ndk-r30`, API 35,
x86_64, AOSP's CMake/Ninja prebuilts and AOSP's `out/host/linux-x86/bin/aidl`.
The NDK's public C Binder headers and `libbinder_ndk` come from its API-35
sysroot. Since r30 does not ship the C++ RAII/interface wrappers, the build
uses the Apache-2.0 header-only snapshot in
`native/third_party/binder_ndk_cpp/`, pinned to AOSP
`frameworks/native` revision `ae266dcb706d083868578cfedce381ef44488a07`.
Those wrappers add no ABI; their C API calls are API-level guarded and compile
for API 35. Binder service-manager registration is platform-only in this NDK:
the public sysroot omits `binder_manager.h` and its service-manager exports.
For that surface the build uses the matching AOSP `include_platform` header
and device-branch vendor `libbinder_ndk.so`; `AServiceManager_addService` is
declared from API 29, below the API 35 target. The rest of each build uses the
NDK sysroot headers and libraries. This split avoids linking host libraries
and keeps the generated AIDL C++ helpers header-only.
Both out-of-Soong scripts default to four workers and cap `MATON_BUILD_JOBS`
at four to preserve memory for the shared build host.
It builds the shared helper first, then every `native/<name>/CMakeLists.txt`,
staging outputs at `prebuilt/native/<name>` and bundle inputs at
`buildinfra/native-built/<name>`. A daemon with local AIDL sources in
`native/<name>/aidl/` gets generated NDK backend sources and headers through
`MATON_AIDL_*`; link `binder_ndk` when using generated Binder interfaces.

For platform HAL contracts, list frozen stable AIDL snapshots in
`native/<name>/stable-aidl.list`, one tab-separated row per package:
`<package> <version> <AOSP source root>`. Use only a published
`aidl_api/<package>/<version>` directory with its `.hash`; never use the mutable
`current/` tree. Include the complete transitive dependency closure, using the
exact versions in that interface version's `versions_with_info` in its
`Android.bp`. For example, the current frozen audio-core v4 dependency closure is:

```text
android.hardware.audio.core	4	hardware/interfaces/audio/aidl
android.hardware.audio.common	5	hardware/interfaces/audio/aidl
android.hardware.audio.core.sounddose	4	hardware/interfaces/audio/aidl
android.hardware.audio.effect	4	hardware/interfaces/audio/aidl
android.hardware.common	2	hardware/interfaces/common/aidl
android.hardware.common.fmq	1	hardware/interfaces/common/fmq/aidl
android.media.audio.common.types	5	system/hardware/interfaces/media
android.media.audio.eraser.types	2	system/hardware/interfaces/media
```

Confirm this list against audio-core v4's `versions_with_info` whenever the
platform branch changes. The frozen snapshots contain the imported package
sources and their own hashes; the generator checks that every configured path
and hash exists. `build-native.sh` passes all frozen roots as AIDL import
paths and runs the AOSP `aidl` host tool with `--lang=ndk`, `--structured`,
`--stability=vintf`, the interface version, and the snapshot hash. It generates
every source in each pinned snapshot into `MATON_STABLE_AIDL_GENERATED_DIR` and
its headers into `MATON_STABLE_AIDL_INCLUDE_DIR`; compile those generated `.cpp`
files into the HAL implementation and link `binder_ndk`, plus the other shared
libraries listed by that interface. `MATON_STABLE_AIDL_COUNT` reports the
number of pinned packages. This lets standalone API-35 NDK daemons implement
interfaces such as `IModule`/`IConfig` without making their source a Soong
module.

Put each HAL's VINTF fragment in `native/<name>/vintf/*.xml`. The NDK build
stages it under `buildinfra/native-built/vintf/<name>/`; add a `file` entry in
`bundle/contents.list` targeting `etc/vintf/manifest/`. Android's runtime ODM
manifest scan discovers it without a Soong `vintf_fragments` stanza.

The NDK sysroot provides `liblog`, `libbinder_ndk` and system-property APIs.
Use the AOSP NDK sysroot for all compile/link inputs so the
binary targets API 35 rather than host libraries.

`tools/build.sh` runs the Gradle and NDK scripts, then packs `odm.img` before
the AOSP step. App/daemon source changes update staged files only. Adding an
app requires a fixed Soong import; adding a driver requires bundle registry,
init rc, ODM file-context coverage, and any required fixed SELinux/property
rules, but no native Soong module.

## Fixed Soong imports and privileges

`buildinfra/Android.bp` contains one `android_app_import` per image app
(Settings, MatonOSRecents) and each associated `prebuilt_etc` permission
file. Sleepd, Wi-Fi, input, Bluetooth, PipeWire, and `libmatonos-ipc` are in
the ODM bundle. AudioToneTest is in the Gradle registry for `adb install`
testing only; it is not an image package. Imported APKs are privileged on
`system_ext`, set `preprocessed: true`, and use their own app-specific signing
keys. Do not set `certificate: "PRESIGNED"`; Soong interprets that Make
convention as a dependency on its PRESIGNED certificate file.

Every privileged permission requested by an imported app must appear in its
own `privapp-permissions-<package>.xml`, imported by a stable `prebuilt_etc`
module under `/system_ext/etc/permissions`. Keep permission names scoped to
the package; do not add broad grants to `platform.xml`.

Android init reads driver rc files from `/odm/etc/init/`; they start daemons without creating control sockets. Every daemon binary uses the fixed `matonos_driver_exec` pattern and shared `matonos_driver` domain in `sepolicy/matonos/`. The fixed vendor `service_contexts` labels all `vendor.matonos.channel.IChannel/<target>` services as `matonos_channel_service`. System Bridge may find and call these services, and drivers may callback to its listener Binder. See this directory's `SHARED-CHANGES.md` for shared source-reference updates.

## System Bridge API and authorization

The system bridge package is `org.matonos.systembridge`, platform-signed,
privileged and persistent. It is the only custom app compiled with platform
APIs. Its versioned AIDL exposes `getBridgeApiVersion`/`getBridgeApiHash`,
generic `call(target, command, jsonArgs)`, topic subscribe/unsubscribe
callbacks, and launcher operations for overlay access, recent tasks, task
switch/close/fullscreen, scoped shelf navigation (`navigateBack`,
`navigateHome`, `navigateRecents`), bounded PNG thumbnails, and the typed
navigation-provider adapter/state query. Task list reads use `REAL_GET_TASKS`;
task removal revalidates current-user recents and refuses MatonOS
Shell/Shelf/Recents and SystemUI tasks. The platform-signed bridge owns the
system application-overlay host and its navigation inset source; it embeds
the selected provider with `SurfaceControlViewHost`. Provider selection is
disabled by default (Shelf is the preset), requires explicit user confirmation,
pins the selected signing certificate, and can be revoked. The
`input.set_absolute_pointer_mode` call disables acceleration and sets pointer
speed to -7, returning the effective settings. The launcher methods authorize the `launcher` target on
every call; navigation methods require the exact Shelf package, and task
methods require one of the exact Shell/Shelf/Recents packages. Mutations and
thumbnails revalidate current-user recents. Every authorized operation and
denial is written to the bridge audit log.

The app-facing bridge API is v6. This app-facing AIDL version/hash is distinct
from the frozen stable vendor channel, which remains VINTF v1. `adb shell content call --uri
content://org.matonos.systembridge.shell --method trust --arg <package>
--extra targets:s:launcher,audio` grants developer access; use `untrust` or
`list` to revoke or inspect entries. The exported provider accepts shell UID
2000 and root only, and audits denied callers. The Trusted developer apps
screen is the user-confirmed grant/revoke and package block-list UI.

Before every privileged operation, the bridge checks:

1. `org.matonos.permission.SYSTEM_BRIDGE` (`signature|privileged|development`),
2. either a built-in signing-certificate and exact package/target match, or a
   revocable runtime entry matching package, current certificate SHA-256 and
   requested target.

All denials are logged with UID, operation and reason. Do not grant target
access using wildcards. Add one exact `target package.name` row only after the
area's app package and signing key are known.

### Stable daemon channel

`vendor.matonos.channel` is a frozen stable AIDL interface (VINTF v1) with
`IChannel.call(command, jsonArgs)`, `subscribe(topic, listener)`, and
`unsubscribe(topic, listener)`. `IChannelListener.onEvent(topic, json)` is
oneway. Its fixed source is `buildinfra/channel/aidl/`; the API snapshots and
hash live in `buildinfra/aidl_api/vendor.matonos.channel/`. `buildinfra/Android.bp`
contains the single `aidl_interface` declaration, enabling Java for the system
bridge and NDK for daemons. The app-facing System Bridge AIDL remains separate
and unchanged; Gradle clients consume that API through `MatonosClient`.

Payloads are JSON strings bounded to 64 KiB, valid UTF-8, at most 32 nested
levels and 8192 values. The shared helper rejects malformed JSON and duplicate
object keys. Each service registers as
`vendor.matonos.channel.IChannel/<target>` using Binder NDK. Its command
handler table remains area-owned, while `matonos_ipc_publish()` fans out JSON
events to Binder listeners. `SystemBridgeService` authorizes permission,
certificate and exact target/package for every call and subscription, then
uses `ServiceManager.checkService()`; absent optional hardware returns quickly
and never holds boot waiting for a daemon.

The ODM bundle includes one `vendor.matonos.channel` AIDL manifest with the
sleep, wifi, bluetooth, input, audio and camera instances. These vendor HAL
instances are optional: they are not added to a framework-required matrix, and
an unregistered service does not block boot or the bridge. The current libvintf device-manifest compatibility check validates device
metadata and framework-matrix requirements; it does not reject additional
vendor HAL declarations solely because the framework matrix has no entry for
them.
The current manifest has a live audio instance from the PipeWire proxy and
reserves camera until its daemon is implemented; `checkService()` returns null
for absent instances. When an area is implemented, register its service through
the helper and retain its manifest instance. Do not add per-daemon control sockets or socket SELinux rules. The audio HAL uses a separate PCM data socket to PipeWire; it is unrelated to daemon control.

The fixed SELinux set includes one `service_contexts` file for the channel
service type. The driver domain has a single-instance registration rule; the bridge uses
`find` plus `binder_call`, and the driver also gets a narrow reverse Binder
call permission for asynchronous listener callbacks. That context file is a
one-time addition because service-manager name mapping cannot be expressed by
file contexts. Socket `connectto` access is removed.

### Stable interface evolution

Keep command/event additions inside JSON so area features do not alter the
stable Binder interface. If transport semantics require a signature change,
create and review a new stable AIDL version, update its API dump/hash and VINTF
version, and bump the app-facing bridge API only if its contract changes.
`build-native.sh` reads the fixed helper's `stable-aidl.list`, invokes AOSP's
`aidl` host tool against frozen snapshots, compiles generated NDK sources into
`libmatonos-ipc.so`, and links `binder_ndk`. HAL projects use the same list
format for their frozen platform AIDL dependencies and ship their VINTF
fragments through `bundle/contents.list`.

### Adding an area daemon and settings page

1. Add `native/<area>/` with a CMake project and NDK daemon. For platform HAL
   interfaces, pin frozen snapshots in `stable-aidl.list`; reserve `aidl/` for
   daemon-owned interfaces.
2. Register command handlers with the helper. Keep names lowercase and
   bounded. Return a JSON object; publish state changes with
   `matonos_ipc_publish(server, "state", json)`. Do not put hardware access in
   the bridge.
3. Add a `file` row for the staged executable and `libmatonos-ipc.so` to
   `bundle/contents.list`; add its rc file and any configs there as well.
4. Add the target instance to the shared ODM channel manifest and start the
   service from its ODM rc. Do not declare an init socket.
5. Add one per-target package line to
   `systembridge/res/raw/target_caller_allowlist.txt`, and implement a typed
   wrapper in `buildinfra/client/` for that area's `call` and event schema.
6. Use `matonos_driver` and the existing `matonos_channel_service` mapping in
   `sepolicy/matonos/`. The bridge is the only client allowed to find the
   generic channel service. Never grant apps direct vendor service access.
7. Add the page to the existing Settings shell without moving hidden framework
   calls into the Gradle app. Verify the daemon and app on a fresh image.

The bridge pilots implement `sleep.get_state` and sleep `state` events, Wi-Fi
`get_state`/`list_devices`/`select_device` and a `state` event, and Bluetooth
`list_devices`/`select_device`. The Settings client has typed sleep and Wi-Fi
wrappers. Other areas should use their own bounded JSON schema, document
commands/topics, and request exact target allowlist entries with their package
names.

## Validation checklist

The build sequence is `tools/build-apps.sh`, `tools/build-native.sh`, then the
normal `tools/build.sh -K -M`. Verify the APK certificate with
`apksigner verify --print-certs`, the installed package is under
`/system_ext/priv-app`, the generated allowlist digest matches the APK, and
the privapp XML is in `/system_ext/etc/permissions`. After a fresh boot, check
`dumpsys package org.matonos.settings` and `org.matonos.shell`, bridge logs
(`MatonSystemBridge`), sleepd logs (`matonos-sleepd`), read sleep state from
Settings, and observe a state event after changing
`persist.vendor.maton.sleep_idle_s`. Check shell task listing, task-fronting,
fullscreen, and overlay authorization from its UI as well.

For QEMU, after a successful full image build, boot a fresh VM with:

```sh
tools/run-qemu-live.sh -g none -m 4096 -a 5565 \
  -s ~/Documents/aosp/out/pc-logs/buildinfra/serial.log
```

Then run these checks from the host:

```sh
adb -s 127.0.0.1:5565 root
adb -s 127.0.0.1:5565 shell setprop persist.vendor.maton.sleep_idle_s 0
adb -s 127.0.0.1:5565 shell am start -n org.matonos.settings/.MainActivity
adb -s 127.0.0.1:5565 shell dumpsys package org.matonos.settings
adb -s 127.0.0.1:5565 logcat -d -s MatonSystemBridge matonos-sleepd
```

The Settings page must show sleep state; press its BACK button to exercise the
privileged input method. The shell should replace the stock launcher and show
its recent-task shelf; selecting a task exercises task-fronting/fullscreen and
opening the shelf exercises overlay authorization. On QEMU, `suspendSupported` may be false and a real
suspend cycle is not required. Shut down that VM before starting another.

## Test on real PCs

The 2026-09-25 16:49 coordinated image boots to `sys.boot_completed=1`.
On that image the bridge, Settings and shell packages are installed, the
Settings package holds `SYSTEM_BRIDGE`, and the audio/bluetooth/input/sleep/wifi
stable channel service instances are registered. The bridge process remained
alive. This image predates the API v4 and ContentProvider changes: the shell
provider lookup returned “Could not find provider”. The Settings activity was
started but a credential lock screen remained in front, so it could not be
used to exercise the startup dialog or visual bridge features. Serial and
screen captures are under `out/pc-logs/buildinfra/` (`serial-current.log`,
`settings.png`, `settings-unlocked.png`).

The Gradle registered-app build succeeds, including the RN shell and refreshed
MatonosClient AIDL generation. `tools/build-native.sh` also builds every
registered daemon, HAL and `libmatonos-ipc.so` for x86_64/API 35. Preflight
passes. The API v4 bridge Java source and privileged permission import are
awaiting the coordinator's `MatonSystemBridge` module compile check and a
subsequent image. No fresh-image evidence is claimed for the new task, overlay,
pointer-setting, trust-provider or ban-list methods yet.

For each machine below, install a freshly built image (do not update the
running system in place), boot normally, and open **MatonOS Settings**. Set
`persist.vendor.maton.sleep_idle_s` to `0` while doing the first checks so an
idle timeout does not interrupt them. The page should connect through
System Bridge and show the sleepd state; its BACK button should inject BACK.
Change the timeout property to a positive value and confirm the displayed
state event updates. Restore it to `0` before continuing normal setup.

- **Build PC (Ryzen 5800X, RX 6600, Intel 7265 Wi-Fi/Bluetooth):** run all
  bridge and event checks above. Verify the desktop remains responsive while
  the property changes, and that sleepd resumes and publishes state after a
  manually initiated suspend. These checks exercise the main development PC;
  Wi-Fi and Bluetooth presence should not affect the bridge path.
- **Surface Pro 3 (Marvell 88W8897 Wi-Fi/Bluetooth, Intel HDA):** run the
  bridge checks, then test lid-close suspend and wake. After wake, confirm the
  Settings page reconnects and reports `sleeping: false`; also test a short
  power-button press and verify that Android receives the configured desktop
  behavior. Repeat with no network connection to confirm bridge operation is
  independent of Wi-Fi/Bluetooth readiness.
- **HP ProDesk 600 G1 (Haswell):** run the bridge checks, then set a short
  positive idle timeout, wait for automatic suspend, and wake with a keyboard
  or mouse. Confirm the new input resets the idle timer and the state event
  reports the resumed state. Test a short power-button press as well; this
  desktop has no lid switch.

On all three PCs, also verify a clean no-daemon failure path by stopping
sleepd during a development boot and confirming Settings displays the daemon
unavailable state without crashing. Restore the normal image afterward. Keep
the timeout at zero until suspend and wake behavior has been confirmed on
that machine.

## Open issues

- The app signing keys are development keys held outside git. Production
  releases need an offline release-key ceremony and a planned certificate
  rotation path.
- Binder callbacks are tied to the daemon service lifetime; clients should
  resubscribe after a daemon restart. Typed client calls return an unavailable
  result if an optional service is absent; the bridge never waits for it.
- Additional daemon target allowlist rows and typed wrappers must be added
  with each area, using exact package names and each app's own signing key.

## Developing system apps

`MatonOSSettings` and `MatonOSRecents` use the shared `MatonosClient` Gradle
library. The default is `BridgeMode.REQUIRED`: call
`MatonOS.requireBridge(activity, accessTargets, requiredChannelTargets,
client -> ...)` before building the app UI. It checks installation, permission,
caller trust, bridge API version, and requested channel presence. Failure is
shown in the common localized dialog and the Activity exits cleanly. A build
that needs developer approval can offer **Trust this app…** when the user has
enabled Developer options and **Allow apps to request bridge trust** on the
bridge's Trusted developer apps screen.

Use `BridgeMode.OPTIONAL` for a cross-platform app that can run without
MatonOS. For example:

```java
MatonosClient client = MatonOS.init(this, BridgeMode.OPTIONAL);
client.addAvailabilityListener((available, reason) -> updateFeatureButton(available));
MatonosClient.Result<String> result = client.call("audio", "get_state", "{}");
if (result.available) showAudioState(result.value);
else showFeatureUnavailable(result.reason);
```

An app may also call `MatonOS.requestTrust(activity, "audio")` at any time.
Its returned request handle accepts `onResult(outcome)`; the result is
`GRANTED`, `DENIED`, or `UNAVAILABLE` with a reason. On a grant, active clients
refresh and their availability callbacks report the newly available bridge.
The startup dialog uses this same request path. It opens the confirmation
screen with the installed package, current signing-certificate SHA-256, and
requested targets prefilled. The app cannot grant itself access. The user
must confirm; trust is revocable from the Trusted developer apps page or with the shell content provider. If requests are disabled, the result explains the setting
and includes the `adb shell content call --uri content://org.matonos.systembridge.shell --method trust --arg <package> --extra targets:s:<comma-separated-targets>`
hint.

Developer trust is available on all builds when Developer options are on.
`adb shell content call --uri content://org.matonos.systembridge.shell --method trust --arg <package> --extra targets:s:<comma-separated-targets>` records the package,
current signer SHA-256, and target set, and grants
`org.matonos.permission.SYSTEM_BRIDGE`. `untrust` with the package revokes that grant; `list` prints trusted entries.
The same provider supports `ban` and `unban`; these override the ODM default
package/certificate deny list. A banned caller sees the bridge as absent,
cannot make calls, and cannot request trust. The default list is
`buildinfra/bridge-banlist.txt`, installed at `/odm/etc/matonos/bridge-banlist.txt`
so point updates do not change the Soong graph. Add only reviewed package or
package-plus-SHA256 entries there. Trust and denial decisions are logged. The
bridge screen lists and revokes entries and contains the
**Allow apps to request bridge trust** switch, off by default and shown only
with Developer options enabled. Trust preferences and settings live in
private systembridge storage/settings, so factory reset clears them. There is
no persistent notification.

The client library localizes REQUIRED-mode startup errors in its resource
strings. Area apps should pass only the targets they actually call and only
require channels whose presence is essential to launching the app. Typed
wrappers return `MatonosClient.Result<T>` so OPTIONAL apps can handle missing
hardware or a missing bridge without exceptions.
