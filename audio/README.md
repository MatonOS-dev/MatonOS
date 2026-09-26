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

An ODM oneshot service runs at `post-fs-data`, with a three-second init
timeout. It scans `/proc/asound/cards` in order and picks the first card with
a playback-capable PCM in `/proc/asound/pcm`, skipping cards named Loopback
or Dummy. Before publishing the selection, it enumerates TinyALSA mixer
controls and sets every playback volume control to its hardware maximum and
every playback switch to `On`. Missing or unsupported controls are ignored;
the selector runs asynchronously and cannot gate HAL startup. It writes
`persist.vendor.audio.primary.card` and
`persist.vendor.audio.primary.device`; BayLibre reads these when its primary
mixer/module is initialized. The input uses the same card/device because this
HAL version has no separate primary capture selector property.

No generated policy, bind mount, detector readiness gate, or `snd-aloop`
loader is shipped. If there is no card, the selector fails/times out, or it
cannot read ALSA state, init enables the HAL's paced output and simulated
input stub properties. The service is asynchronous and does not gate HAL
startup or boot. The HAL's static policy still has the full module list, so
module registration does not depend on detection. The stock-derived stub
implementation has previously been observed to fill capture with
pseudo-random data; do not claim the no-card mic produces zero samples until
verified or replaced.

## HDA codec driver preload

The image can discover `snd_hda_intel` by PCI modalias before the HDA bus
codec modules have registered. An asynchronous `post-fs` init action runs
`/system/bin/modprobe` against `/vendor/lib/modules/modules.dep` for each
shipped `snd-hda-codec-*.ko`, using depmod's dependency ordering. It then
tries to remove and reload `snd_hda_intel`, prompting the controller to
enumerate codecs with their drivers present. Failures to load optional codecs
or reload the controller are ignored; audio service and boot startup never
wait for this action. Without an HDA controller, the selector and HAL retain
their no-card fallback.

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
  -x "-audiodev wav,id=snd0,path=$HOME/Documents/aosp/out/pc-logs/audio/ovmuz.wav -device intel-hda -device hda-duplex,audiodev=snd0"
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

The BayLibre source was synced successfully at the pinned revision. The ODM
bundle/config changes still need a new coordinator-built image before runtime
claims can be made. Previous playback attempts were on older stock-HAL and
loopback images and do not verify this implementation.
