# Audio status — BayLibre HAL integration

## Implemented

- BayLibre generic AIDL HAL pinned at `a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5`; no upstream source changes.
- Product uses `com.android.hardware.audio.generic`; policy is schema 7.0 with `default`, `r_submix`, `usb`, and `stub`, and no BT/LE audio. Coordinator reports fresh image 22:31 as OK, with BayLibre replacing the stock APEX.
- The asynchronous selector scans `/proc/asound/cards` and `/proc/asound/pcm`, skips Loopback/Dummy, and chooses the first playback card. It now invokes TinyALSA `tinymix` to set playback volume controls to reported hardware maximum and playback switches to `On`. Individual control failures remain non-fatal.
- Static verification passed: `sh -n` for the selector, XML parsing, schema 7.0 validation, ODM bundle assembly (4.1 MB), and `tools/check-selinux-labels.sh` (all nine init-started vendor programs have exec labels).

## Runtime verification

I booted the 22:31 image with HDA and WAV capture on private slot/port 5556. QEMU reached the UEFI MatonOS menu, but ADB remained offline for about three minutes; no Android boot properties or ALSA state could be read. The WAV is only its 44-byte header, so this is not playback evidence and no peak can be reported. I stopped only my own QEMU process; the user's VM was not touched.

Still required on a successful fresh boot: `sys.boot_completed=1`, selector card/device properties, `dumpsys media.audio_flinger` output thread, playback of `ovmuz.mp3` to non-silent WAV, no-card boot, and selector-failure boot. Inspect the serial/boot path before treating the HDA/no-output result as an audio failure.

## Volume boost

The requested +20% software boost is not implemented. BayLibre's primary stream clamps hardware volume to `[0, 1]`, and its mixer backend maps that range to at most 100%. The AOSP media volume curve has no limiter; a positive endpoint would not safely amplify above unity through this path. The media curve remains at 0 dB at its maximum. A maintained BayLibre fork needs a software gain stage with a limiter before +1.6 dB can be enabled. No measured WAV peak is available.

## Product switch follow-up

The first image build's VINTF check found both stock and generic audio APEX manifests; the coordinator then reported a successful 22:31 image with the generic HAL replacing stock. `audio/SHARED-CHANGES.md` contains the exact final `pc_x86_64.mk` package filter diff in case the duplicate returns.

## 09:15 image follow-up — HDA preload did not work yet

The 09:15 image was tested on private QEMU port 5556 with Intel HDA and WAV
capture. With a headless virtual display (`-g virgl`), Android reached
`sys.boot_completed=1`, but `/proc/asound/cards` still showed no soundcards,
the selector reported `vendor.maton.audio.selector_result=no_card`, and the
kernel logged `snd_hda_intel ... Cannot probe codecs, giving up`. The controller
and HDA core modules were loaded, but no `snd_hda_codec_*` modules were
present. The captured WAV was 0 bytes; no playback was proven. The no-display
`-g none` attempts remained ADB-offline, so they do not count as no-card
boot verification.

The prior preload used `/system/bin/modprobe` and ran via `exec_background`.
I changed it to an asynchronous init oneshot service and `/vendor/bin/modprobe`
with `/vendor/lib/modules/modules.dep`. Its executable runs in
`matonos_driver`, and a `domain_auto_trans` to `vendor_modprobe` gives the
module helper its intended SELinux domain. It loads every shipped
`snd-hda-codec-*.ko`, then tries to reload `snd_hda_intel`; all failures remain
non-fatal and do not gate HAL registration or boot. Shell syntax and ODM bundle
assembly passed. This adds an existing-policy domain transition, so a fresh
full image is required before testing this version.

Build requested: `audio: full image — async ODM post-fs preload of all shipped
snd-hda-codec modules through modules.dep, then re-probe snd_hda_intel; run the
helper in vendor_modprobe via the driver-domain transition; verify HDA card,
selector and WAV playback plus no-card boot`. Pending coordinator image and
fresh-boot verification. No peak measurement is available.
