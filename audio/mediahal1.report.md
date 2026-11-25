# mediahal1 report — 2026-11-25

## Result and limits

This task is **not complete**. Phase A has a source-compiled software provider,
configuration, VINTF ownership, packaging checks and tests, awaiting server
link/package/runtime validation. Phase B's capture conversion and phase C's
RAOP/Snapcast discovery integration remain unfinished. Their prerequisites
and concrete gaps are recorded below. ROC is excluded by the requested
permissive-licence condition.

All writes and commits were in `/home/hanro50/matonos/build/dt-media`, branch
`wip/media-hal`. Nothing under `/mnt/data/aosp` was modified. No keys were
printed, copied or committed. No push, Soong, Gradle, kernel build, VM,
hot-patching, image packaging or Bluetooth playback test was performed.

## Phase A — software Bluetooth audio

Commits:

- `ebdf8334` — Add software-only Bluetooth audio provider to media HAL.
- `23585321` — Package Bluetooth provider licence with a local Soong licence module.

Files:

- `audio/bt-audio/`: source fork, Android.bp, NOTICE, PROVENANCE.md,
  README.md, factory tests and source compilation helper.
- `audio/pipewire-hal/Android.bp`: whole-link software factory; remove direct
  shared stock-provider dependency; use `--as-needed` for the stock core
  archive's unused shared-provider dependency.
- `audio/native-hal/vintf/manifest.xml`: Bluetooth audio v6 factory in the
  media APEX's existing manifest.
- `audio/audio.mk`: five unicast profile gates, broadcast source/assistant,
  HAP and software-only offload properties.
- `tools/build-audio-apex.sh`: omit any bootstrap copy of the shared stock
  provider; reject a HAL that still imports it; package NOTICE/provenance.
- `audio/testapp/app/src/main/AndroidManifest.xml` and
  `.../org/matonos/audiotest/BluetoothStatusActivity.java`: public-API probe.
- `audio/README.md`: current status and links.

### Design and verified source contracts

AOSP's existing `ModuleBluetooth` creates the factory using
`createIBluetoothAudioProviderFactory()` in the audio HAL process. The fork
preserves that ABI and the shared BluetoothAudioSession/FMQ transport
(the transport is FMQ; no new IPC protocol is added). The existing
`main.cpp` already creates the Bluetooth IModule through
`Module::createInstance`, and the existing policy already contains A2DP,
hearing aid, LE unicast and LE broadcast routes. Those paths were inspected
and retained.

The factory constructs software A2DP encode/decode, hearing aid encode,
LE unicast encode/decode, LE broadcast encode/decode and HFP encode/decode.
It advertises PCM capabilities only for that explicit list. Offload,
UNKNOWN and unrecognised enum values have empty capabilities and cannot be
opened. It does not advertise hardware codec provider info. All nine session
types are **proven by code reading and compilation only**; none was run.

Missing-FMQ and registration failures log context. Death-link failure logs
session/status and fails startup; unlink uses the actual link cookie rather
than upstream's incorrect `this`, with status logging. NDK's `onUnlinked`
owns cookie cleanup. Upstream's protocol-derived double-buffer/FMQ sizing
was retained; no new job/thread limit or arbitrary device/session cap was
introduced.

The final-link assumption is guarded: the APEX builder fails if the HAL
still has DT_NEEDED `android.hardware.bluetooth.audio-impl.so`. The complete
HAL link and this guard have **not** been executed here. A source-object
compile does not prove this link behavior. The shared audio-session utility
still includes its existing upstream codec/offload helper code; the factory
does not expose hardware sessions through it.

### LE Audio support gates

Read in the checked-out Bluetooth module:

- `framework/java/android/bluetooth/BluetoothAdapter.java`: the public API
  reports an error when Bluetooth is unavailable/off; otherwise calls the
  stack and returns FEATURE_SUPPORTED or FEATURE_NOT_SUPPORTED.
- `android/app/src/com/android/bluetooth/btservice/AdapterServiceBinder.java`:
  `isLeAudioSupported` requires every `Config.LE_AUDIO_UNICAST_PROFILES` entry.
- `.../Config.java`: those are LE_AUDIO, VOLUME_CONTROL,
  CSIP_SET_COORDINATOR, MCP_SERVER and LE_CALL_CONTROL.
- Their service `isEnabled()` methods use BluetoothProperties. Exact
  property names were checked in
  `system/libsysprop/srcs/android/sysprop/BluetoothProperties.sysprop`.
- `.../AdapterService.java`, `updateLeAudioProfileServiceState`: removes
  unicast profiles if the controller lacks CIS-central support. Broadcast
  source additionally depends on extended/periodic advertising and ISO
  broadcaster features. Assistant has its own controller capability gates.
- `system/bta/le_audio/codec_manager.cc`: hardware offload requires
  `ro.bluetooth.leaudio_offload.supported`; it is explicitly false here.

No overlay is needed for these profile gates in this revision. System-owned
properties belong in product configuration, not in vendor APEX init.
No controller capabilities are spoofed. Consequently the API acceptance is
unverified and requires suitable controller hardware. It cannot honestly be
promised on every generic PC or while Bluetooth is off. Old controllers may
still show LE Audio unsupported despite the software provider; they lack
radio ISO transport, which software LC3 cannot add.

### Licences and SELinux

New forked code: Apache-2.0, from clean hardware/interfaces revision
`0162af698935100a590b7359581ac8b1b80693e5`; headers retained. New test/probe
code: Apache-2.0. No additional third-party library was introduced. A local
licence module is used because the upstream hardware_interfaces licence is
visible only to its own subpackages. NOTICE and provenance are packaged in
`share/licenses/matonos-bluetooth-audio`.

No SELinux rule/type/file-context additions. Existing stock rules suffice
by source inspection: `IBluetoothAudioProviderFactory/default` is labelled
`hal_audio_service`; `hal_audio_default` is a hal_audio server; Bluetooth is
already a hal_audio client with service lookup and Binder calls. Existing
FMQ/shared-memory rules apply. Enforcement has not been tested. Linux apps
receive no PipeWire socket permissions.

### Why no-controller startup does not introduce the historical boot wait

The factory is present in the same process as ModuleBluetooth before the
IModule is registered. Its constructor does no controller discovery, service
wait, stream open or codec startup. The stock module's availability check is
`AServiceManager_checkService`, not waitForService. FMQs and sessions are
created on client requests. Bluetooth has no attached/default policy route;
the primary no-device fallback remains available. No init service, critical
flag, synchronous exec, hardware wait or startup dependency was added.
The existing asynchronous media service starts remain in place. A complete
fresh no-controller boot is still required to verify image-wide behavior;
this is a source argument, not a runtime guarantee for an unbuilt image.

### Validation actually performed

- NDK r30/API 35 compiled all seven provider translation units and
  `FactoryTest.cpp` to objects with `-std=c++20 -Wall -Werror`, using the
  checkout's generated Bluetooth audio v6/common/FMQ/media headers. PASS.
  Temporary objects were removed. No final link or test execution claimed.
- `javac -Xlint:all -Werror` compiled the existing tone activity and new
  Bluetooth probe against the API 35 public android.jar. PASS.
- AOSP `bpfmt` formatted both touched Android.bp files. PASS.
- `bash -n tools/build-audio-apex.sh`, XML parsing and `git diff --check`.
- Preflight with host tools last in PATH ran all seven checks. It failed on
  six existing, absent worktree APK inputs: LinuxSubsystem bootstrap,
  Fennec, Thunderbird, MicroGmsCore, MicroGCompanion and MicroGGsfProxy.
  No new module/declaration/formatting failure was reported. Preflight is
  **not passing** until the coordinator supplies those existing inputs.

Reproduce source compilation without Soong:

```sh
cd /home/hanro50/matonos/build/dt-media
python3 audio/bt-audio/tests/check-sources.py --aosp /mnt/data/aosp --ndk /home/hanro50/Documents/android-ndk-r30
env PATH="$PATH:/mnt/data/aosp/out/host/linux-x86/bin" MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh
```

### Coordinator tests — not run

Run these on the server checkout after integrating the commits. Use its
configured paths/keys; do not move keys into the repository. Only the
coordinator runs builds. Build the native test module through the normal
coordinator build path, as well as the full media APEX/image; test module
name is `matonos_bluetooth_audio_software_test`. It is not an image package.

After the bootstrap HAL link, verify:

```sh
readelf -d out/target/product/pc_x86_64/vendor/bin/hw/matonos-audio-hal
```

There must be no stock provider DT_NEEDED entry. Let the standard image
pipeline package the media APEX and check its compatibility sidecar contains
Bluetooth audio v6/default. The builder guard must pass. Run the normal
`tools/check-selinux-labels.sh` on the built product and require no FAIL.

On a fresh private no-controller VM, using its own ADB serial:

```sh
ADB=/path/to/aosp/out/host/linux-x86/bin/adb
SERIAL=127.0.0.1:5556
"$ADB" -s "$SERIAL" root
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" shell setprop persist.vendor.maton.sleep_idle_s 0
"$ADB" -s "$SERIAL" shell getprop sys.boot_completed
"$ADB" -s "$SERIAL" shell getprop init.svc.vendor.audio-hal-aidl
"$ADB" -s "$SERIAL" shell service list
"$ADB" -s "$SERIAL" shell dumpsys media.audio_policy
"$ADB" -s "$SERIAL" shell dumpsys media.audio_flinger
"$ADB" -s "$SERIAL" logcat -b all -d -s MatonBtAudioFactory BtAudioAIDLService AHAL_ModuleBluetooth AHAL_MainPipeWire
"$ADB" -s "$SERIAL" logcat -b all -d | grep 'avc: denied'
```

Require boot completion and primary playback with the provider/default and
IModule/bluetooth registered despite the missing controller. No new Bluetooth
route should be active. Check all declared modules, effects and existing local
speaker playback for regression.

Execute the separately built native test (test-data deployment only, never
replace system files):

```sh
"$ADB" -s "$SERIAL" push out/target/product/pc_x86_64/data/nativetest64/matonos_bluetooth_audio_software_test/matonos_bluetooth_audio_software_test /data/local/tmp/matonos_bluetooth_audio_software_test
"$ADB" -s "$SERIAL" shell chmod 0755 /data/local/tmp/matonos_bluetooth_audio_software_test
"$ADB" -s "$SERIAL" shell 'LD_LIBRARY_PATH=/apex/com.matonos.hardware.media/lib64:/vendor/lib64 /data/local/tmp/matonos_bluetooth_audio_software_test'
```

Record linker/SELinux failures separately from assertions; this test's device
execution environment is unverified. Both tests must pass before calling the
factory behavior runtime-verified.

Build the existing `audio/testapp` on the coordinator's Gradle path, install
its APK as test data, enable Bluetooth on appropriate hardware, then:

```sh
"$ADB" -s "$SERIAL" shell am start -W -n org.matonos.audiotest/.BluetoothStatusActivity
"$ADB" -s "$SERIAL" logcat -d -s MatonBtAudioProbe
"$ADB" -s "$SERIAL" shell dumpsys bluetooth_manager
"$ADB" -s "$SERIAL" shell getprop bluetooth.profile.bap.unicast.client.enabled
"$ADB" -s "$SERIAL" shell getprop ro.bluetooth.leaudio_offload.supported
```

The probe prints actual public-API results and FEATURE_SUPPORTED's value;
it does not infer success from properties. Require the unicast result to
match FEATURE_SUPPORTED on an ISO-capable enabled controller and check the
Settings LE Audio control. With no controller/off, an unavailable/off result
is expected, while boot and local audio must still work. Actual classic,
LE unicast capture/output, broadcast, hearing aid, HFP and hotplug tests are
listed step by step in `bt-audio/README.md`; all remain unverified.

## Phase B — webcams via PipeWire: unfinished

Commit `5a3899a9` adds `camera/pipewire-provider/README.md` only. No provider
fork, capture code, dependency, VINTF change, policy rule or camera.mk switch
was made. The existing stock provider remains the fallback.

The source inspection found that this checkout's implementation is under
`camera/provider/default`, not `provider/aidl/default`. V4L2 is embedded in
provider discovery, device metadata enumeration, session configuration and
queueing, and V4L2Frame memory mapping/offline ownership. A discovery-only
replacement would advertise cameras that cannot capture. The missing
transport rewrite is detailed in the README. The PipeWire recipe also
explicitly disables V4L2/libcamera and removes their monitor scripts.

No libcamera SPA clean build was attempted. No licence or kernel change was
introduced. This phase remains unfinished engineering work, not an
established need for an AOSP patch. Boot behavior is unchanged because the
stock fallback remains selected. The README gives baseline commands and
future capture/hotplug/no-camera tests; none was run here.

## Phase C — network speakers: unfinished; ROC excluded

Commit `edd94cfd` adds `audio/network-speakers/README.md` only. No network
module config, discovery service, library, init change or allow rule was
added. RAOP and Snapcast acceptance is not met.

At the exact PipeWire pin, both stock discover modules require avahi-client;
RAOP sink also requires OpenSSL. The existing recipe supplies neither this
Avahi integration nor a compatible discovery service. Android's current NSD
is a Java service, not an Avahi D-Bus endpoint; merely enabling modules
cannot reuse it. The no-new-daemon direction is NsdManager from an existing
trusted Android service plus explicit sink add/remove glue; that integration
has not been written. Stock Avahi modules instead require additional daemon/
D-Bus infrastructure, or a discovery fork using embedded Avahi core. No
unverified dependency recipe was enabled in the working audio path.

ROC is omitted because upstream's MPL-2.0 does not meet the explicit
permissive-licence condition. Sources and dependency notes are linked in the
network README. This exclusion is not about GPLv3. No new linked dependency
or licence obligation was added by this phase.

No network SELinux rules were added for absent clients. The README lists
operations that a completed design would need and why NSD reuse avoids
multicast permissions in PipeWire. It also records the no-network/no-speaker
boot requirement, sink enumeration, HAL mirroring, playback, IPv4/IPv6,
network-change and removal tests. All are unverified. Current startup remains
unchanged; no network receiver can gate boot through these documentation
changes.

## Shared changes and kernel

No external SHARED-CHANGES request is required for phase A: all its changes
are in this worktree and must be integrated together. The coordinator must
build product properties and the new VINTF entry into the base image as well
as rebuild the APEX; an APEX update alone cannot add the system properties.
Phases B/C have no hidden shared-file change request standing in for code.
No AOSP source patch, new kernel option, kernel module or Mesa change is
requested. Do not mark the media APEX feature set complete from this report.
