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
`-g none` attempts remained ADB-offline. A later no-card boot using `-g virgl`
and no HDA device also remained at the UEFI handoff with ADB offline for over
three minutes; it was stopped as my own VM. Neither attempt counts as successful
no-card boot verification.

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


## Controller/codec-link investigation (coordinator finding)

Coordinator confirmed the same `Cannot probe codecs, giving up` result with
QEMU `intel-hda` and `ich9-intel-hda`; this points to HDA controller/codec
communication rather than a missing codec driver. The kernel config includes
`CONFIG_SND_HDA_INTEL=m`, `CONFIG_SND_HDA=m`, generic and HDMI codec modules,
and modules are staged for ueventd modalias loading. The `snd_hda_intel`
module exposes `enable_msi`, `single_cmd`, `probe_mask`, `position_fix`, and
`model` parameters.

The previous `audio/modules.options` file only contained `snd_aloop` options
and was not wired into the kernel-module image, so it had no runtime effect.
I updated `audio/BoardConfig.mk` to install that file as
`/vendor/lib/modules/modules.options`, and changed it to `options snd_hda_intel
enable_msi=0`. This is a scoped first diagnostic: disabling MSI can help
controller interrupt compatibility while preserving codec address discovery.
I did not set `probe_mask=1`, which could suppress other codecs on generic
PCs. If the next image still has no codec, test `single_cmd=1` separately and
check the QEMU HDA model/codec through its monitor; `position_fix` is not an
initial codec-probe remedy.

At the time of this initial note, the option had not yet been tested. The
subsequent 10:06 image and coordinator report below confirm HDA enumeration
with the option enabled. No kernel rebuild was required; the relevant
controller, codec, and core options are already enabled as modules.


## 10:06 image — MSI option restores HDA enumeration (coordinator report)

The module options packaging change is in the 10:06 image: the generated
`/vendor/lib/modules/modules.options` contains `options snd_hda_intel enable_msi=0`. Coordinator tested its windowed QEMU VM with HDA and reports
that `/proc/asound/cards` now contains the card, the selector result is
`found`, and AudioFlinger has a speaker output thread. This confirms that the combined controller-option and preload change restored
enumeration. Read-only inspection of that VM reports
`vendor.maton.audio.codec_preload=done`, `enable_msi=0`, the generic analog
card with playback and capture PCM 00-00, all shipped HDA codec drivers in
`lsmod`, and selector card/device `0`/`0`. Since both changes landed in the
same image, this does not isolate whether MSI disable or the successful
preload/reprobe was decisive. The preload remains best-effort and never gates
boot. No kernel rebuild was needed; controller and codec
modules/configuration were already present.

My private headless QEMU runs on the same image did not reach ADB after the
UEFI boot handoff, both with HDA and without an audio device. The
`hda-duplex` + WAV run also printed `Could not create a backend for voice adc`;
changing to QEMU's output-only `hda-output` removed that ADC warning
but did not change the ADB-offline boot. Those WAV files remained empty, so I
have not independently verified audible playback or measured its peak. A
no-card boot and WAV playback still need a successful private fresh boot.
