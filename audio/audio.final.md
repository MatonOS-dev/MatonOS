# Audio status — BayLibre HAL integration

## Changes in this pass

- Kept the BayLibre generic AIDL HAL pinned at `a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5`; no source modifications or Soong edits were made here.
- Extended the asynchronous boot selector to enumerate the chosen card's TinyALSA controls. Playback volume controls are set to their reported hardware maximum and playback switches to `On`; absent controls and individual failures are ignored.
- Verified selector shell syntax, policy XML parsing, policy schema 7.0 validation, and ODM bundle assembly. Bundle check output: `/tmp/audio-volume-check.img`, 4.1 MB.

## Volume boost status

The +20% software boost is not implemented. BayLibre's primary stream accepts hardware volume only in `[0, 1]` and maps the mixer setting to at most 100%. The AOSP media volume curve has no limiter; a positive endpoint would not make this backend amplify above unity safely. The media curve remains at 0 dB at its maximum. A maintained BayLibre fork needs a software gain stage with a limiter before +1.6 dB can be enabled.

No WAV playback/peak measurement is available for the current BayLibre image. Runtime checks remain pending a coordinator-built image: HDA playback capture, no-card boot, selector-failure boot, and the measured WAV peak.

## Build request

A full image build is requested for the pinned BayLibre HAL integration and this selector/mixer update. Build status has not yet reported a fresh image for runtime verification.
