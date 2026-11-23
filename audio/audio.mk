# PipeWire is the supported audio HAL; the legacy BayLibre APEX is not in the default image.
MATON_AUDIO_PIPEWIRE_HAL ?= true
PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio audio_effects_config.xml,$(PRODUCT_PACKAGES))
# The generic APEX supplies the effect service (its core service is replaced by
# the ODM vendor.audio-hal-aidl override). Not the stock com.android.hardware.audio
# APEX: it declares IBluetoothAudioProviderFactory, which conflicts with the vintf
# fragment android.hardware.bluetooth.audio-impl installs for the PipeWire HAL.
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
PRODUCT_PACKAGES += android.hardware.audio.service-aidl.matonos
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
