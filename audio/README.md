# MatonOS audio

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
`buildinfra/native-built/pipewire`; `bundle/contents.list` installs that tree
in ODM. `tools/build-pipewire.sh` builds the stack with NDK r30. See
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
`buildinfra/native-built/pipewire`; the bundle installs that tree in ODM. The
NDK build completed previously. `NEEDED.txt` records the staged ELF
dependencies. `audio/pipewire-PROVENANCE.md` documents upstream pins, license
review and compatibility patches.

## PipeWire runtime (P4)

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

The stock audio effect service and effect libraries come from AOSP's
`com.android.hardware.audio` APEX. The BayLibre APEX is not installed in this
mode. `audio/effects/audio_effects_config.xml` uses AOSP's un-suffixed library
filenames that match the stock effect libraries supplied by the APEX. The APEX also contains the stock core service and its init
rc/VINTF fragment. The ODM `vendor.audio-hal-aidl` service uses `override` to
replace that core service; the ODM VINTF override advertises only the
configured PipeWire core modules.

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
