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

## HDA codec preload follow-up

The user VM confirmed the image boots and AudioFlinger has two outputs, but `/proc/asound/cards` was empty because QEMU's HDA controller was present while the codec driver was not. Added an asynchronous post-fs ODM action that uses `modprobe -d /vendor/lib/modules` on each shipped `snd-hda-codec-*.ko` (with dependency ordering from `modules.dep`), then attempts to remove and reload `snd_hda_intel`. It never gates HAL startup or boot; all module errors are ignored. ODM bundle assembly passed with the new rc/script. Fresh image and QEMU verification are pending the coordinator build.
