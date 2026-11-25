# mediahal2 implementation report

Worktree: `/home/hanro50/matonos/build/dt-media`, branch `wip/media-hal`.
Only this worktree was written. No Soong/Gradle build, VM, AOSP source edit,
key access, push, kernel build or Mesa build was performed.

## Result and selection

Network discovery, desired-state forwarding, sink lifecycle/retry and RAOP
crypto integration are implemented. The existing bridge uses NsdManager;
the existing audio HAL forwards commands to an optional module in the
existing PipeWire daemon. That module loads the stock RAOP sink or a Snapcast
integration using the stock protocol-simple module. No Avahi/new daemon.

The camera provider/device/session/utils/offline fork now implements
PipeWire registry discovery, enumerated modes, stream capture, owned frame
storage, raw YUYV/NV12 conversion and hotplug/reconnection. All eight native
translation units compile to NDK objects, including frozen v1 camera Binder
headers regenerated from the checkout. **The final provider link is not
verified, so the stock external provider remains selected by default.**
`MATON_PIPEWIRE_CAMERA=true` is an explicit coordinator opt-in for both the
product build and APEX packer after that link succeeds.

Neither feature is claimed runtime-verified. Optional Roc/libcamera were
not built; their absence is not an AOSP limitation or licence exclusion.

## Commits

Implementation: `dccbb1b656d32d5151d3262ec4462b922e02f5e4`
(`Implement NSD network speakers and PipeWire camera transport`).
This report is recorded in the following documentation commit. No push.

## Files

- `systembridge/src/.../NetworkSpeakerDiscovery.java`, service lifecycle and
  manifest: persistent NSD watches, network loss, duplicates, resolution,
  retry, restart replay; platform NSD owns multicast.
- `audio/network-speakers/NetworkSpeakers.{c,h}`: HAL-side private Unix/
  metadata transport, queued changes, reconnect and monotonic leases.
- `audio/network-speakers/NetworkSpeakerModules.c`: PipeWire module owning
  metadata and optional module load/unload/retry/lease cleanup.
- `audio/network-speakers/module-maton-snapcast.c`: PCM listener module,
  nonblocking JSON-RPC connection, partial writes, response/error handling,
  connection/registration deadline and fd-close gravestones.
- `audio/network-speakers/{Android.bp,raop-compat.h,check-native.sh,
  PROVENANCE.md,README.md}` and audio HAL channel/library wiring.
- `camera/pipewire-provider/src/`: Apache-2.0 stock provider/device/session/
  offline/utils/conversion fork plus plain-C PipeWire transport and close
  logger. See its PROVENANCE for source paths and revision.
- `camera/pipewire-provider/{Android.bp,matonos-camera-provider.rc,
  check-native.py,README.md,PROVENANCE.md}`, `camera/camera.mk`: buildable
  fork target, init/config and conservative selection switch.
- `tools/build-pipewire.sh`: pinned BoringSSL static crypto, RAOP and V4L2
  enabled, no Avahi, custom modules built, monitor retained, required runtime
  artifacts/licences/provenance/DT_NEEDED checks.
- `audio/config/pipewire/.../20-network-speakers.conf`, WirePlumber video
  profile, PipeWire/HAL environment, camera group/network group, APEX labels,
  optional camera payload/VINTF integration in `tools/build-audio-apex.sh`.
- `sepolicy/matonos/matonos_driver.te`: existing fixed policy file only.
  `audio/pipewire-PROVENANCE.md`: updated dependency roles/licences.

## Discovery/property mapping

Source: PipeWire 1.6.9 commit
`8fa27cabdc6c0c1350c69c026af5850ef0af1e26`,
`src/modules/module-raop-discover.c` and `module-snapcast-discover.c`.

| Resolved input/TXT | Property/value |
| --- | --- |
| address, port, service name, hostname, local domain, interface | `raop.ip`, `.port`, `.name`, `.hostname`, `.domain`, `.ifindex` |
| `device` | `raop.device` unchanged |
| `tp` | `raop.transport`: prefer UDP→`udp`, then TCP→`tcp`, otherwise original value |
| `et` | `raop.encryption.type`: prefer 5→`fp_sap25`, 4→`auth_setup`, 1→`RSA`, otherwise `none` |
| `cn` | `raop.audio.codec`: prefer 0→PCM, 1→ALAC, 2→AAC, 3→AAC-ELD, otherwise unknown |
| `ch`, `sr` | `audio.channels`, `audio.rate` unchanged |
| `ss` | `audio.format`: 16→S16, 24→S24, 32→S32, otherwise UNKNOWN |
| `am` | `device.model` unchanged |
| Snapcast address/port/name/hostname/domain/interface | `snapcast.ip`, `.port`, `.name`, `.hostname`, `.domain`, `.ifindex`, `.ifname`; `local.ifname` |

NSD hostname is normalized to its local-domain FQDN. Network LinkProperties
supply the interface; IPv6 scope is retained, and RAOP link-local IPs get an
interface suffix when missing. Snapcast follows stock discovery's IPv4-only
selection. Snapcast has no TXT mapping at this pin. Duplicate service types
and network announcements share receiver identity rather than creating
multiple sinks. Removed duplicates are reconciled against surviving entries.

RAOP loads `libpipewire-module-raop-sink` with those properties. Snapcast
loads `libpipewire-module-protocol-simple` with `capture=true`,
`playback=false`, `capture.props={ media.class=Audio/Sink }`, S16LE/48000/2,
FL/FR, a receiver-specific stable node name, and
`server.address=["tcp:<routed-local-IPv4>:0"]`. It reads the actual listening
address from that module, then sends Server.GetRPCVersion,
Stream.RemoveStream and Stream.AddStream over the advertised control port.
The URI is `tcp://<actual-listener>?name=<stable-name>&mode=client&sampleformat=48000:16:2&codec=pcm&chunk_ms=20`.
Users route this stream to Snapcast clients on the server. The server must
be able to connect back to this local listener, as with stock discovery.

Resulting nodes are ordinary Audio/Sink globals, already accepted by
`ModulePipeWire::handleNode`; no new AudioService device mechanism is added.

## Policy and boot safety

AOSP `system/sepolicy/private/hal_neverallows.te` forbids TCP/UDP sockets in
`hal_audio_default`. The initially considered in-HAL socket execution would
fail those neverallows/CTS; it was replaced before committing. There is no
exception attribute, policy bypass or platform patch. IP work runs inside
the existing non-HAL PipeWire daemon. The HAL uses its existing private Unix
connection to publish desired state as metadata.

New rules and reasons:

- `matonos_pipewire` self TCP/UDP creation/use, announced-port connect/bind,
  and local node bind: RAOP RTSP/audio/timing/control; Snapcast JSON-RPC plus
  the stock protocol's ephemeral TCP PCM listener. All port types are used
  because DNS-SD may advertise arbitrary valid receiver ports.
- PipeWire's init supplementary `inet` group permits normal IP sockets.
  No `NET_RAW`, `NET_ADMIN`, mDNS membership or raw/packet socket is granted.
- PipeWire/WirePlumber video-device directory/character access: the monitor
  probes capabilities; the server's V4L2 SPA opens/captures selected webcams.
  Their init groups include camera for DAC access.
- `hal_camera_default`: private-runtime directory search/socket write,
  self Unix stream socket use, PipeWire connectto/fd use/shared-memfd access,
  and vendor/APEX config/library reads. Init uses cameraserver plus audio
  group; the executable keeps stock `hal_camera_default_exec` labeling.
- No Linux app domain receives access to system PipeWire.

No hardware/network/discovery result gates init or Binder registration.
The network controller is `ifexists nofail`; module loads happen on a
PipeWire loop, failures log and retry, and local/fallback nodes remain.
NSD runs on a bridge worker; the channel queues requests. Leases remove stale
state after a lost bridge/HAL, and metadata rebind handles PipeWire restarts.
Camera startup connects asynchronously, absent PipeWire/cameras yields an
empty list, and requested capture has a two-second deadline/disconnect wake.
Frames own copied bytes and return SPA buffers immediately. These source
arguments still need full-image boot/runtime verification.

## Licences and symbols

The rule is **no (L)GPLv3**, not permissive-only. Roc's MPL-2.0 is allowed;
Roc was simply optional and not implemented after the two required sinks.
Dependency licences/pins and inherited camera conversion licences are listed
in both new PROVENANCE files and the updated PipeWire provenance.

BoringSSL `0cd1f6a94b670e6b61af95a06e71e690ae8e3262` built successfully with
the NDK. Every used legacy crypto symbol resolved in the RAOP shared-module
link with `--no-undefined`; the exact list is in network PROVENANCE. There
are no missing required symbols. OpenSSL 3 provider APIs are compiled out.
The only source compatibility needed is explicit bn.h and bionic's removed
index/rindex aliases. The resulting RAOP ELF imports only libpipewire, libm,
libdl and libc; crypto and the NDK C++ support are static.

## Local validation and exact commands

Working directory was the worktree above. Verification scratch was
`.mediahal2-check/`; it was removed after verification. The reproducible
helpers stay in the tree. No complete `build-native.sh`/`build-apps.sh` was
run: the task forbids shared/AOSP writes and Soong/Gradle/VM work, and these
would build unrelated components/write outside this worktree.

```sh
bash audio/network-speakers/check-native.sh \
  /mnt/data/aosp \
  /home/hanro50/Android/Sdk/ndk/30.0.16248370 \
  /home/hanro50/matonos/build/main-out/deps/pipewire/src \
  /home/hanro50/matonos/build/main-out/deps/pipewire/build \
  /home/hanro50/matonos/build/main-out/deps/pipewire/install/pipewire \
  .mediahal2-check

python3 camera/pipewire-provider/check-native.py \
  --aosp /mnt/data/aosp \
  --ndk /home/hanro50/Android/Sdk/ndk/30.0.16248370 \
  --pipewire-prefix /home/hanro50/matonos/build/main-out/deps/pipewire/install/pipewire \
  --out .mediahal2-check/camera-verified

javac -Xlint:all -Werror -d .mediahal2-check/java \
  -cp /home/hanro50/Android/Sdk/platforms/android-36/android.jar:/mnt/data/aosp/out/soong/.intermediates/frameworks/base/framework-minus-apex/android_common/combined/framework.jar:/mnt/data/aosp/out/soong/.intermediates/device/maton/pc_x86_64/buildinfra/vendor.matonos.channel-V1-java/android_common/javac/vendor.matonos.channel-V1-java.jar \
  systembridge/src/org/matonos/systembridge/NetworkSpeakerDiscovery.java

bash -n tools/build-pipewire.sh tools/build-audio-apex.sh audio/network-speakers/check-native.sh
/mnt/data/aosp/out/host/linux-x86/bin/bpfmt -l audio/network-speakers/Android.bp camera/pipewire-provider/Android.bp audio/pipewire-hal/Android.bp
git diff --check
PATH=/mnt/data/aosp/out/host/linux-x86/bin:$PATH MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh
```

Network helper PASS: compiled HAL transport object; built pinned BoringSSL
crypto; linked controller, Snapcast and stock RAOP with undefined-symbol
checks. Camera helper PASS: generated frozen v1 common/device/provider
headers and compiled all eight translation units, warnings as errors.
Java PASS. Shell/format/diff checks PASS.

Additional V4L2 checks (PASS), with the same local paths as above:

```sh
NDK=/home/hanro50/Android/Sdk/ndk/30.0.16248370
PWSRC=/home/hanro50/matonos/build/main-out/deps/pipewire/src
PWBUILD=/home/hanro50/matonos/build/main-out/deps/pipewire/build
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang
"$CC" -std=gnu11 -fPIC -shared -Wall -Werror -Wno-unused-function \
  -Wno-unused-parameter -Wl,--no-undefined -I"$PWBUILD" -I"$PWSRC/spa/include" \
  "$PWSRC/spa/plugins/v4l2/v4l2.c" "$PWSRC/spa/plugins/v4l2/v4l2-device.c" \
  "$PWSRC/spa/plugins/v4l2/v4l2-source.c" -lm -o .mediahal2-check/libspa-v4l2.so
curl -fLsS https://raw.githubusercontent.com/illiliti/libudev-zero/7a6eee2db11f2eb0f7fd065ae9597fcd270e6734/udev.h \
  -o .mediahal2-check/libudev.h
mkdir -p .mediahal2-check/v4l2-config
sed 's/#undef HAVE_LIBUDEV/#define HAVE_LIBUDEV 1/' "$PWBUILD/config.h" \
  > .mediahal2-check/v4l2-config/config.h
"$CC" -std=gnu11 -fPIC -Wall -Werror -Wno-unused-function -Wno-unused-parameter \
  -fsyntax-only -I.mediahal2-check/v4l2-config -I.mediahal2-check \
  -I"$PWSRC/spa/include" "$PWSRC/spa/plugins/v4l2/v4l2-udev.c"
```

The existing core-plugin config disables udev; the separate monitor syntax
check enables it only in the scratch copy. Full udev-enabled plugin/stack
linking remains unverified.

Preflight FAILS on six absent existing APK inputs in this worktree:
MatonLinuxSubsystemBootstrap, MatonFennec, MatonThunderbird, MatonMicroGmsCore,
MatonMicroGCompanion, MatonMicroGGsfProxy. After supplying bpfmt on PATH it
reports no additional issues. Those unrelated APKs were not copied or altered.

## Coordinator build/VM acceptance; unverified items

1. Restore the six normal prebuilt APK inputs on the build server. Run the
   complete updated PipeWire/WirePlumber recipe, policy build and provider
   link. Use `MATON_PIPEWIRE_CAMERA=true` consistently only after link success;
   then pack the media APEX and full fresh image. Run the standard label and
   VINTF/APEX dependency checks. Do not deploy through hot-patching.
2. Fresh boot on an agent-owned VM, no USB/VFIO passthrough. Example after
   the coordinator has supplied its image/normal launcher configuration:

```sh
tools/run-qemu-live.sh -g none -m 4096 -a 5556 -s <coordinator-log-dir>/mediahal2-serial.log
ADB=<checkout>/out/host/linux-x86/bin/adb
SERIAL=127.0.0.1:5556
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" root
"$ADB" -s "$SERIAL" shell setprop persist.vendor.maton.sleep_idle_s 0
"$ADB" -s "$SERIAL" shell getprop sys.boot_completed
"$ADB" -s "$SERIAL" shell dumpsys media.camera
"$ADB" -s "$SERIAL" shell dumpsys media.audio_policy
"$ADB" -s "$SERIAL" logcat -b all -d -s MatonNetworkAudio MatonPipeWireCamera ExtCamPrvdr ExtCamDevSsn AHAL_AudioControlChannel
"$ADB" -s "$SERIAL" logcat -b all -d | rg 'avc: denied|PipeWire|Snapcast|RAOP'
```

3. Use the coordinator's audio UID/private-runtime command runner (root may
   be intentionally rejected by the private PipeWire access policy):

```sh
XDG_RUNTIME_DIR=/data/vendor/maton-audio \
PIPEWIRE_RUNTIME_DIR=/data/vendor/maton-audio \
LD_LIBRARY_PATH=/apex/com.matonos.hardware.media/lib64 \
/apex/com.matonos.hardware.media/bin/pw-cli ls Node
```

Require Video/Source and remote Audio/Sink nodes when appropriate; no-camera/
no-network boot must finish and local audio must work. User-mode QEMU NAT
normally cannot browse the host LAN's multicast advertisements or accept the
Snapcast server's callback connection, so use the coordinator's appropriate
LAN-connected test environment or real hardware for that acceptance.

4. Run the detailed raw/MJPEG preview, JPEG, FPS/size, flush/offline,
   disconnect/replacement, IR/metadata filtering, server/HAL/bridge restart,
   network duplicate/loss and receiver playback tests in the two READMEs.

Unverified: full recipe/Meson reconfiguration, udev-enabled plugin final
link, WirePlumber monitor startup, final camera/HAL/bridge platform linkage,
sepolicy/neverallow compilation and AVCs, APEX signing/packaging/VINTF,
fresh-image boot, permissions under enforcing policy, NSD on actual networks,
receiver compatibility/audio quality, IPv6/link-local behavior, Snapcast
routing/callback, device mirroring/routing/fallback, all camera capture/
conversion/timing/hotplug/offline/privacy behavior on real hardware, and
private-runtime denial to Linux apps. libcamera/Roc were not built.
No SHARED-CHANGES request, AOSP patch or kernel change is required by this code.
