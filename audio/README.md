# MatonOS audio

## Design

The HAL is BayLibre's generic AIDL audio implementation at
`hardware/baylibre/audio`, pinned in `manifest/maton.xml` to
`a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5` (branch `main`, synced 2026-09-25).
It builds in Soong. AudioFlinger performs the mixing; BayLibre's primary
module opens ALSA through TinyALSA.

The supplied source calls its APEX `com.android.hardware.audio.generic`,
despite its README documenting the name `com.android.hardware.audio.baylibre`.
Product configuration installs the actual upstream module name and filters
the stock `com.android.hardware.audio` APEX out. Renaming the APEX and its
service paths requires a BayLibre fork; we do not edit the imported project.
The APEX includes the effect service, so the stock APEX is not needed for its
effect factory. `audio_effects_config.xml` remains installed.

The imported service fragment advertises audio core v3 and includes
Bluetooth. Our ODM VINTF override replaces its core declaration with the
modules configured here and excludes Bluetooth/LE audio. This AOSP checkout
has frozen `android.hardware.audio.core` through v4; v5 does not exist in the
source tree, so the override advertises v4. The APEX's audio effect factory
declaration remains intact.

The static schema-7.0 policy config includes exactly four modules:

- `default` — the primary ALSA endpoint, registered as `IModule/default`.
- `r_submix` — system remote submix.
- `usb` — USB audio hotplug ports.
- `stub` — an available fallback module.

No Bluetooth or LE-audio policy is included. ODM and vendor fallback policy
use the same module set. `mixer_controls.xml` uses BayLibre's generic Master,
headphone/PCM/speaker, and capture control names; absent controls are
gracefully unavailable.

## Boot card selection

After the asynchronous HDA codec preload/reprobe completes, an ODM oneshot
selector scans `/proc/asound/cards` in order and picks the first card with a
playback-capable PCM in `/proc/asound/pcm`, skipping Loopback and Dummy. It
publishes the persistent BayLibre card/device properties and its result before
touching mixer controls, so the three-second init timeout cannot turn slow
mixer IO into a false `no_card` result. Integer playback volumes start at
two-thirds of hardware range and playback switches are unmuted with `1`;
unsupported controls are ignored. The input uses the same card/device because
this HAL version has no separate primary capture selector property.

No generated policy, bind mount, detector readiness gate, or `snd-aloop`
loader is shipped. If there is no card, the selector fails/times out, or it
cannot read ALSA state, init enables the HAL's paced output and simulated
input stub properties. The service is asynchronous and does not gate HAL
startup or boot. The HAL's static policy still has the full module list, so
module registration does not depend on detection. The stock-derived stub
implementation has previously been observed to fill capture with
pseudo-random data; do not claim the no-card mic produces zero samples until
verified or replaced.

PipeWire is currently unshipped. `tools/build-pipewire.sh` only stages
experimental prebuilts under `prebuilt/pipewire`; the inert
`maton-pipewire.rc` documents why no service is started. There is no ODM
bundle installation row or verified runtime integration. Do not enable the
Linux-app audio path until binaries, libraries, configuration and proxy are
installed under ODM and verified on a fresh boot.

`audio/native-hal/` is also unshipped reference work. It is not installed or
registered by the product. Its source still registers a `bluetooth` module,
which intentionally does not match the shipped ODM VINTF (`default`,
`r_submix`, `usb`, `stub`); do not treat it as a usable alternative HAL.

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

## Upstream differences and limitations

We make no changes to `hardware/baylibre/audio`. The upstream APEX name and
its embedded core VINTF fragment do not match this request exactly: the APEX
is named `com.android.hardware.audio.generic` rather than
`com.android.hardware.audio.baylibre`, and the embedded manifest advertises
v3 plus Bluetooth. The ODM override addresses the module declarations and
uses v4, which is the newest frozen core AIDL version in this AOSP checkout.
A maintained MatonOS fork may rename the APEX, remove the unused Bluetooth
instance from the embedded fragment, and update it to the target interface
version when AIDL v5 is available.

The selector chooses the first playback card exactly as requested; it does
not prioritize analog over HDMI. If a GPU HDMI card appears first, it may be
selected. A later selector enhancement can prefer analog after this basic
path is proven.

The requested software +20% gain is not implemented. BayLibre's primary
stream clamps hardware volume to `[0, 1]`, and its mixer backend maps that
range to at most 100%; the AOSP policy volume curve sends a level to that
clamped interface and has no limiter. Raising the curve's endpoint would
therefore not safely provide the boost. The normal media curve remains at
0 dB at its top rather than claiming an unsafe or ineffective boost. A future
maintained BayLibre fork needs a software gain stage with a limiter before
+1.6 dB can be enabled. No WAV peak has been measured for this build.

## Verification

After the requested fresh image is available, use one private headless QEMU
slot (5556 or higher; never modify the user's VM). For each boot, set
`persist.vendor.maton.sleep_idle_s 0` and confirm `sys.boot_completed=1`,
that only the BayLibre audio APEX is installed, and that
`dumpsys media.audio_policy`/`dumpsys media.audio_flinger` show registered
modules and a primary output thread.

With HDA attached, inspect `/proc/asound/cards` and `cat /proc/asound/pcm`,
confirm the selector chose the first playback-capable non-loopback card, play
`out/pc-logs/agents/test-media/ovmuz.mp3` through Fossify Music or an ACTION_VIEW
intent, and capture the QEMU WAV using:

```sh
bash tools/run-qemu-live.sh -g none -m 4096 -a 5556 \
  -s "$HOME/Documents/aosp/out/pc-logs/audio/hda-serial.log" \
  -x "-audiodev wav,id=snd0,path=$HOME/Documents/aosp/out/pc-logs/audio/ovmuz.wav -device ich9-intel-hda -device hda-output,audiodev=snd0"
```

Check nonzero WAV samples and confirm playback position reaches the end.
Repeat with no audio hardware using `-x "-device virtio-rng-pci"`; boot must
complete and the HAL must register its configured modules and use paced stub
streams. Also test a deliberately failing selector (for example, invoke the
service with an unreadable ALSA proc path in a test build) and verify
`sys.boot_completed=1`. Check audioserver/system_server logs for watchdogs.

## Real hardware checks

1. Build PC (Ryzen 5800X/RX 6600, Intel 7265 Wi-Fi/BT, Realtek): record card
   order, selected PCM, HDA/HDMI output, mic capture, and USB headset attach.
2. Surface Pro 3 (Marvell 88W8897, Intel HDA): confirm card selection and
   document any SOF firmware/topology needs separately.
3. HP ProDesk 600 G1 Haswell: confirm selected card and HDA/HDMI playback.
4. Disable all audio devices and confirm the system still boots and the HAL
   remains registered.

## Current test state

The 2026-09-26 09:15 image boots Android with QEMU HDA attached, but HDA
detection failed: `snd_hda_intel` logged `Cannot probe codecs, giving up`,
`/proc/asound/cards` was empty, and the selector reported `no_card`. The first
preload attempt used `/system/bin/modprobe`; the revised service now uses
`/vendor/bin/modprobe` with an explicit transition to `vendor_modprobe`. The
revision needs a fresh image and runtime verification. The 09:15 QEMU run did
not produce playback evidence: its WAV was empty/header-only. No-card fresh
boot and actual playback remain unverified for this BayLibre image.
