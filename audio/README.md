# MatonOS audio

## Updateable vendor audio APEX (audioapex1)

The default PipeWire audio stack is packaged as one signed, prebuilt vendor
APEX, `com.matonos.hardware.audio`. Its payload contains PipeWire,
WirePlumber, ALSA-lib/UCM, GLib/GIO, libffi, libudev-zero, the Soong-built
PipeWire HAL, AOSP effects service and its configured generic effect
libraries, `audio_effects_config.xml`, init rc, and VINTF fragments. The APEX payload version is a UTC build timestamp;
`tools/build-audio-apex.sh` increments it for each package. The payload key
and container signing keys are machine-local under `MATON_RELEASE_KEY_DIR`.
The payload key must remain stable across staged updates. The release key
provisioning work will add this APEX key to the release `apex-keys` mapping.

`tools/build.sh` uses two AOSP passes: the bootstrap product installs the
Soong HAL/effects executables, then host `apexer` packages those binaries with
the NDK-built PipeWire stage and the final pass imports the generated APEX
through `prebuilt_apex`. `audio/apex/.find-ignore` hides that prebuilt module
until the APEX exists. The ODM bundle continues to carry audio selector and
codec-preload glue; PipeWire, HAL/effects services and their VINTF fragment
are no longer bundle contents.

The APEX rc file declares services only. The vendor init trigger in
`init.pc_x86_64.rc` starts them at `post-fs-data`, after APEX activation and
/data setup. Runtime variables are limited to HOME and XDG/PipeWire runtime
paths under `/data`; compiled prefix, config paths, linker namespace and SPA
plugin paths use `/apex/com.matonos.hardware.audio`. HAL and effect-service
VINTF declarations name this APEX with `updatable-via-apex`. SELinux labels
for all executable paths are fixed in both `audio/apex/file_contexts` and
the existing `sepolicy/matonos/` policy files, so updates keep the same
labels.

No AOSP build or VM boot was run in the isolated worktree. On the server,
build a fresh image and check `apexd`/`/apex` show the APEX, all four services
are running, `pw-cli ls Node` sees the ALSA card, and `IModule/default` plus
`IFactory/default` are registered. Test a staged update using a higher-version APEX signed by the same payload
and container keys: run `adb install --staged /path/to/com.matonos.hardware.audio.apex`,
reboot, then check `adb shell cmd apexservice getActivePackages`.

## Design

The default HAL is MatonOS's PipeWire-backed AIDL implementation in
`audio/native-hal/`, installed by the Soong module in `audio/pipewire-hal/`.
AudioFlinger performs mixing; the HAL forwards PCM to a system-wide PipeWire
server, and WirePlumber manages ALSA devices. Set `MATON_AUDIO_PIPEWIRE_HAL`
in the environment for both image and bundle builds. It defaults to `true`;
setting it to `false` selects the existing BayLibre backend and its policy and
effect config. The AOSP `com.android.hardware.audio` APEX supplies the stock
AIDL effect factory and effects libraries. Its core init service is overridden
by the MatonOS HAL
service, while the ODM VINTF fragment advertises only the modules in the
active schema-7.0 policy: `default`, `r_submix`, `usb`, `stub`, and
`bluetooth`. The core interface version is v4, the latest frozen version in
this checkout.

The Bluetooth audio module is static and controller-independent. The HAL
registers the AOSP Bluetooth audio provider when that module is created; the
ODM manifest advertises `IBluetoothAudioProviderFactory/default`. With no
controller or paired endpoint, no Bluetooth route is active.

The ODM audio selector and codec preload remain in the bundle, but they do
not choose PipeWire's endpoint: WirePlumber uses libudev-zero to enumerate
`/sys`, monitor kernel uevents and discover ALSA devices through
`/proc/asound` and `/dev/snd`.

## Legacy selector and codec preload

The boot-time ALSA selector and HDA codec preload remain in the bundle for
explicit BayLibre fallback builds. PipeWire does not consume the selector's
card properties; WirePlumber discovers ALSA cards and follows hotplug events
through libudev-zero. These services are asynchronous and do not gate audio
startup.

PipeWire 1.6.9 and WirePlumber 0.5.17 are staged under
`buildinfra/native-built/audio-apex`; the APEX build consumes that tree. `tools/build-pipewire.sh` builds the stack with NDK r30. See
`audio/pipewire-PROVENANCE.md` for revision pins, license review and patch
rationale.

## HDA controller options and codec driver preload

The 09:15 image loaded `snd_hda_intel`, but QEMU reported `Cannot probe
codecs, giving up`; loading generic codec modules later could not fix the
controller/codec communication failure. The `audio/modules.options` draft was
not connected to kernel module packaging, so it never appeared under
`/vendor/lib/modules` and could not affect ueventd's initial controller
probe. `audio/BoardConfig.mk` now sets `BOARD_VENDOR_KERNEL_MODULES_OPTIONS_FILE`, which installs this file as
`/vendor/lib/modules/modules.options`. It starts with `options snd_hda_intel
enable_msi=0`, a diagnostic compatibility setting for QEMU/PC interrupt
routing. It deliberately does not force `probe_mask=1`, which could hide
additional codec addresses on physical PCs. On the 10:06 image, the coordinator confirmed a QEMU HDA card appears, the
selector reports `found`, and AudioFlinger opens its speaker output. This image includes `enable_msi=0`, which is consistent with the interrupt-mode
workaround restoring codec detection. If this result is not reproducible on a
fresh private boot or physical PCs, test `single_cmd=1` as a separate
follow-up; `position_fix` and `model` affect DMA/board setup rather than the
controller's initial codec response.

An asynchronous `post-fs` init service also runs `/vendor/bin/modprobe` for
each shipped `snd-hda-codec-*.ko`, using `/vendor/lib/modules/modules.dep`,
then tries to reload `snd_hda_intel`. The service runs in `matonos_driver`; the
`vendor_toolbox_exec` transition sends modprobe into Android's narrow
`vendor_modprobe` domain. This can retry detection after codec bus drivers are
registered, but it cannot fix a controller that cannot communicate with any
codec. Failures remain non-fatal and never gate HAL startup or boot. Without
an HDA controller, the selector and HAL retain their no-card fallback.

## Current test state

The default PipeWire path has not yet been built into a fresh image or verified
at runtime. Previous HDA/QEMU results apply to the legacy BayLibre backend.

## PipeWire vendor build status (2026-11-23)

`tools/build-pipewire.sh` builds PipeWire 1.6.9, WirePlumber 0.5.17,
ALSA-lib/UCM, GLib/GIO, libffi and libudev-zero for x86_64/API 35. It stages
runtime files, configs, UCM profiles and licenses under
`buildinfra/native-built/audio-apex`; `tools/build-audio-apex.sh` packages that
tree with the Soong HAL and effects binaries. The
NDK build completed previously. `NEEDED.txt` records the staged ELF
dependencies. `audio/pipewire-PROVENANCE.md` documents upstream pins, license
review and compatibility patches.

## Previous PipeWire runtime design (superseded by audioapex1)

`MATON_AUDIO_PIPEWIRE_HAL` defaults to `true`. `audio/maton-pipewire.rc` starts
PipeWire as the `audio` user after `/data` is ready, with XDG and PipeWire
runtime sockets under `/data/vendor/maton-audio`, with writable state under
`/data/vendor/maton-audio/state`. WirePlumber starts after
PipeWire's init service reaches running; the audio HAL starts in that same
property action. PipeWire and WirePlumber have separate SELinux domains
defined in the existing fixed `sepolicy/matonos/` policy files. The HAL runs
in `hal_audio_default`, reads the same runtime directory and connects to the
PipeWire server. Device-less systems retain the PipeWire fallback null sink
and the primary module's `No audio device` ports, so discovery failure does
not gate boot. libudev-zero enumerates `/sys`, reads `/proc/asound`, and uses
kernel uevent netlink for hotplug; no system udev daemon is required.

The effect service comes from the BayLibre `com.android.hardware.audio.generic`
APEX; `audio/effects/audio_effects_config.xml` names its `*_generic` placeholder
libraries plus the real AOSP AIDL effects installed to `/vendor/lib64/soundfx`
(device.mk). The APEX also contains a core service; the ODM
`vendor.audio-hal-aidl` service uses `override` to replace it, and the ODM VINTF
override advertises only the configured PipeWire core modules. AOSP's stock
`com.android.hardware.audio` APEX is not used: it declares
`IBluetoothAudioProviderFactory/default`, which the vendor vintf fragment of
`android.hardware.bluetooth.audio-impl` (linked by the PipeWire HAL) already
declares, and the duplicate fails `check_vintf`.

The ODM bundle installs the two audio init rc files, the audio-core VINTF
fragment (`audio/native-hal/vintf/manifest.xml`), all PipeWire runtime
files/configs, and the policy pair selected by `tools/build-bundle.sh`.
The Soong-built HAL installs as `/vendor/bin/hw/matonos-audio-hal`; the HAL rc
starts it only after the PipeWire service is running.

Source checks for this step: shell syntax checks, XML parsing, Android.bp
formatting when `bpfmt` is available, effect-library/config reconciliation,
and review of init/bundle paths. A full image build and fresh-boot check are
coordinator-owned and have not been run from this worktree. On a fresh image,
verify `dumpsys media.audio_flinger`, `pw-cli ls Node`, and logcat tags
`AHAL_MainPipeWire`, `PipeWire`, `WirePlumber`, `EffectFactory`, and
`AudioFlinger`; the coordinator report gives exact commands.

## Fresh image and hardware verification

On the next coordinator-built image, verify `sys.boot_completed=1`, registered
modules in `dumpsys media.audio_policy` and an output thread in
`dumpsys media.audio_flinger`. Run `pw-cli ls Node`; confirm an ALSA sink when
a card exists and `maton-fallback` when none exists. Play a short PCM track and
verify it advances and produces nonzero QEMU WAV samples. Check logcat for
`AHAL_MainPipeWire`, `PipeWire`, `WirePlumber`, `EffectFactory`,
`AudioFlinger`, linker errors and SELinux denials. Confirm the stock effect
factory registers and that configured effects are visible to AudioFlinger.

Repeat on the Ryzen 5800X/RX 6600/Realtek PC, Surface Pro 3 (Marvell 88W8897
and Intel HDA), and HP ProDesk 600 G1 (Haswell). On each, test analog/HDA,
HDMI and USB output where available, microphone capture where present, and
USB audio hotplug. Also boot with all audio devices disabled; boot must finish
and the PipeWire fallback must remain available.

## PipeWire device mirroring and output boost (P3)

The primary AIDL module monitors PipeWire registry nodes and connects one
external AIDL device port per `Audio/Sink` and `Audio/Source` node. The port
address carries a stable MatonOS identity and the port name uses PipeWire's
node description. A stream follows the device selected by AudioPolicy through
its connected-device address; the HAL maps that identity to the current PipeWire
registry ID and passes it as the stream target. The HAL disconnects and
reconnects a live stream when AudioPolicy changes the connected device. The P2 static speaker/microphone stand-ins
are now external ports connected only while their direction has no real
PipeWire node. Discovery setup is non-fatal: the module connects those
stand-ins before opening the PipeWire registry and retains them if discovery
fails.

Device identity uses the first available of `device.bus-path`,
`device.serial`, `device.name`, then `node.name`, combined with
`device.profile.name`, `node.name`, and input/output direction. `node.name`
distinguishes multiple ports from one physical device. A stable FNV-1a 64-bit key
is exposed as 16 lowercase hex digits. It intentionally excludes PipeWire
node IDs and ALSA card numbers; if a device exposes none of the stable
properties, its node name is the documented last-resort identity.

MatonOS Settings calls the existing `IChannel/audio` service using
`get_devices` and `set_boost`. The HAL persists the small key/multiplier map in
`/data/vendor/maton-audio/device-boosts.conf`; the Settings app never writes a
vendor data file or audio property directly. A new identity defaults to 1.0×.
The 0.5×–1.5× multiplier is applied to the active PipeWire stream's
`SPA_PROP_volume` when the stream exposes that control, leaving AudioFlinger's
PCM mixing and volume path in place. If PipeWire rejects that control, the HAL
applies the same multiplier to PCM samples. No limiter is applied. The stopgap session-0 DynamicsProcessing controller and
its Settings Secure preference have been removed.

### P3 source checks

- `bpfmt -d audio/pipewire-hal/Android.bp` passed.
- Python `ElementTree` parsing passed for both PipeWire audio policy XML files.
- `bash -n audio/maton-audio-select.sh audio/maton-audio-codecs.sh` passed.
- NDK r30 `clang++ -fsyntax-only -Wall -Wextra -Werror` passed for
  `AudioControlChannel.cpp` using the generated channel/audio AIDL headers.
- The full HAL source syntax check and runtime/image verification remain
  pending: this worktree has no staged PipeWire public headers. No AOSP build,
  VM, or adb commands were run by this agent.

### Coordinator fresh-image verification

Run the following on the next coordinator-built image with no USB audio device
attached. Keep `persist.vendor.maton.sleep_idle_s=0` for the test session.

```sh
tools/run-qemu-live.sh -g none -m 4096 -a 5573 \
  -s /mnt/data/aosp/out/pc-logs/pw-p3/serial.log \
  -x "-device qemu-xhci,id=xhci -monitor unix:/mnt/data/aosp/out/pc-logs/pw-p3/monitor.sock,server,nowait"
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 shell getprop sys.boot_completed
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 shell dumpsys media.audio_policy
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 shell 'XDG_RUNTIME_DIR=/data/vendor/maton-audio PIPEWIRE_RUNTIME_DIR=/data/vendor/maton-audio PIPEWIRE_CONFIG_DIR=/odm/etc/pipewire LD_LIBRARY_PATH=/odm/lib64:/odm/lib64/pipewire-0.3:/odm/lib64/spa-0.2 /odm/bin/pw-cli ls Node'
```

With no real nodes, confirm the connected output and input ports are named
`No audio device` and `No audio device input`. Hotplug a QEMU USB audio card:

```sh
printf 'device_add usb-audio,id=pw_usb_audio,bus=xhci.0\n' | socat - UNIX-CONNECT:/mnt/data/aosp/out/pc-logs/pw-p3/monitor.sock
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 shell dumpsys media.audio_policy
```

Confirm the fallback output port disappears, the USB sink and source appear as
separate connected ports, and MatonOS Settings → Hardware lists the output
sink with a 0.50×–1.50× slider. Change its multiplier and check the persisted
value after leaving and reopening the page. Play audio while changing Android
media volume and the multiplier; verify zero Android volume remains silent and
that the selected device changes without restarting AudioFlinger. Then remove
the card and check that the no-device stand-ins return:

```sh
printf 'device_del pw_usb_audio\n' | socat - UNIX-CONNECT:/mnt/data/aosp/out/pc-logs/pw-p3/monitor.sock
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 shell dumpsys media.audio_policy
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5573 logcat -b all -d -s AHAL_ModulePipeWire AHAL_AudioControlChannel AHAL_StreamPipeWire PipeWire WirePlumber
```

The same checks should be repeated with a fresh boot on the Ryzen/RX 6600,
Surface Pro 3 and HP ProDesk listed above. In particular, verify source
selection and capture, analog/HDA and HDMI nodes, USB replacement, a boot with
no audio hardware, and SELinux denials. The new dynamic generic AIDL device
ports, live PipeWire stream retarget, external-port disconnect behavior, and
stream-volume control still require that runtime validation.
