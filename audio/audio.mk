# PipeWire is the supported HAL; the generic audio APEX is bootstrap-only for extracting its effects binary and libraries.
MATON_AUDIO_PIPEWIRE_HAL ?= true
PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio com.android.hardware.audio.generic audio_effects_config.xml,$(PRODUCT_PACKAGES))
# Bootstrap pass builds the APEX-compiled effects implementation; the final
# product uses only com.matonos.hardware.media and has no generic audio APEX.
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
ifeq ($(MATON_AUDIO_APEX_READY),true)
PRODUCT_PACKAGES += com.matonos.hardware.media
else
# Bootstrap pass: build the Soong HAL and APEX-compiled effects implementation before apexer packages them.
PRODUCT_PACKAGES += \
    android.hardware.audio.service-aidl.matonos \
    com.android.hardware.audio.generic \
    libbundleaidl \
    libdownmixaidl \
    libdynamicsprocessingaidl \
    libhapticgeneratoraidl \
    libloudnessenhanceraidl \
    libpreprocessingaidl \
    libreverbaidl \
    libvisualizeraidl
endif
else
PRODUCT_PACKAGES += com.android.hardware.audio.generic
endif

# Keep ODM and vendor policy copies aligned with the PipeWire HAL modules.

# Keep the vendor fallback policy aligned with the ODM policy.
PRODUCT_COPY_FILES := $(filter-out \
    frameworks/av/services/audiopolicy/config/audio_policy_configuration_generic.xml:% \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:% \
    ,$(PRODUCT_COPY_FILES))
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
PRODUCT_COPY_FILES := $(filter-out \
    device/maton/pc_x86_64/audio/config/audio_policy_configuration.xml:% \
    ,$(PRODUCT_COPY_FILES))
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/pipewire-hal/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml
endif
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config-odm/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
PRODUCT_COPY_FILES := $(filter-out \
    device/maton/pc_x86_64/audio/config-odm/primary_audio_policy_configuration.xml:% \
    ,$(PRODUCT_COPY_FILES))
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/pipewire-hal/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml
endif
# /vendor/etc/r_submix_audio_policy_configuration.xml stays AOSP's Soong module
# (aosp_r_submix_audio_policy_configuration.xml, added by an inherited .mk, so
# a filter-out here can't drop it); the ODM copy is the one policy reads.
PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/stub_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/stub_audio_policy_configuration.xml
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config-odm/mixer_controls.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_controls.xml

# The AudioTrack smoke-test app is a Gradle project in audio/testapp and is
# installed with adb for QEMU checks; it is not an image product package.

ifeq ($(MATON_AUDIO_APEX_READY),true)
PRODUCT_COPY_FILES := $(filter-out device/maton/pc_x86_64/audio/effects/audio_effects_config.xml:%,$(PRODUCT_COPY_FILES))
endif

# mediahal1: all five Config.LE_AUDIO_UNICAST_PROFILES gates, plus broadcast.
# Vendor props like cuttlefish (shared/phone/vendor.prop): the generic system
# image forbids PRODUCT_SYSTEM_PROPERTIES from device makefiles.
# Software codec paths still require controller CIS/BIS support for radio I/O.
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
PRODUCT_VENDOR_PROPERTIES += \
    bluetooth.profile.bap.unicast.client.enabled=true \
    bluetooth.profile.csip.set_coordinator.enabled=true \
    bluetooth.profile.vcp.controller.enabled=true \
    bluetooth.profile.mcp.server.enabled=true \
    bluetooth.profile.ccp.server.enabled=true \
    bluetooth.profile.bap.broadcast.source.enabled=true \
    bluetooth.profile.bap.broadcast.assist.enabled=true \
    bluetooth.profile.hap.client.enabled=true \
    ro.bluetooth.leaudio_offload.supported=false \
    ro.bluetooth.a2dp_offload.supported=false
endif
