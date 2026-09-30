# Shared changes and upstream follow-up

The audio product switch is area-local: `audio/audio.mk` filters the stock
audio APEX and installs the imported BayLibre module. No change to AOSP
projects is made. The BayLibre checkout at
`hardware/baylibre/audio` is pinned to
`a3abeb7aefc1ac665705f96f3cdd3672a98ec3d5`.

## Existing `device.mk` comment cleanup

The comment immediately above the inherited audio policy configs in
`device.mk` still describes the stock example HAL and Bluetooth patch. It is
documentation only; the active behavior is overridden by `audio/audio.mk`.
When the owner next edits that shared file, replace the stale block with:

```make
# BayLibre's generic AIDL HAL loads module instances from the policy XML.
# audio/audio.mk replaces the stock APEX, installs the static schema-7.0 ODM
# policy and vendor fallback, and omits Bluetooth/LE audio.
```

## Product package filter contingency

The 22:16 full-image VINTF check found both
`/apex/com.android.hardware.audio` and
`/apex/com.android.hardware.audio.generic` in the assembled device manifest,
so the early filter in `audio/audio.mk` does not remove the stock APEX after
all inherited product packages are combined. The coordinator reports that
the 22:31 image build passed, and its VINTF staging directory now contains
only the generic APEX. If the stock APEX returns in a later image, apply this
one-line final filter in `pc_x86_64.mk`, immediately after its `device.mk`
inherit and before `PRODUCT_NAME`:

```diff
 $(call inherit-product, device/maton/pc_x86_64/device.mk)
+PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio,$(PRODUCT_PACKAGES))
 
 PRODUCT_NAME := pc_x86_64
```

This removes only the stock core audio APEX; the BayLibre
`com.android.hardware.audio.generic` APEX remains selected. Re-run the
full-image VINTF check to confirm the generic APEX is the only audio-core
provider.

## BayLibre fork follow-up

The synced source's README says `com.android.hardware.audio.baylibre`, but
its actual APEX module and service paths are `com.android.hardware.audio.generic`.
The source APEX fragment also advertises core v3 with Bluetooth. Do not edit
the synced project in this tree. A maintained BayLibre fork should rename the
APEX and service path consistently, remove the Bluetooth core instance, and
update the embedded manifest when the target audio-core AIDL is frozen.

The checked-out AOSP tree currently has frozen `android.hardware.audio.core`
through v4 only. A v5 VINTF declaration cannot be built/verified against this
tree; the product override therefore advertises v4. Move to v5 when the AOSP
interface and BayLibre implementation both provide it.

## Round 2 audit follow-up

- PipeWire is deliberately **unshipped** for now. No `bundle/contents.list`
  rows should be added for `prebuilt/pipewire`; `audio/maton-pipewire.rc` is
  inert and `tools/build-pipewire.sh` only stages experimental artifacts. To
  ship later, add explicit ODM bundle rows for the binaries, libraries,
  configs/data and `maton-pipewire-proxy`, then validate startup and Linux-app
  audio on a fresh image before enabling an init rc.
- `bundle/contents.list` currently installs
  `audio/config-odm/mixer_controls.xml` to `/odm/etc/mixer_controls.xml`, but
  BayLibre reads this file only from `/vendor/etc`. Remove that `file` row;
  `audio/audio.mk` already installs the same source at
  `/vendor/etc/mixer_controls.xml`.
