# The PipeWire AIDL HAL stays opt-in until its init and SELinux handoff lands.
MATON_AUDIO_PIPEWIRE_HAL ?= false
PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio,$(PRODUCT_PACKAGES))
ifeq ($(MATON_AUDIO_PIPEWIRE_HAL),true)
PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio.generic,$(PRODUCT_PACKAGES))
PRODUCT_PACKAGES += android.hardware.audio.service-aidl.matonos
else
# Keep the current BayLibre service as the default until P4.
PRODUCT_PACKAGES += com.android.hardware.audio.generic
endif

# Keep ODM and vendor fallback policy copies aligned with the HAL's modules,
# including the controller-independent Bluetooth audio module.

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
    device/maton/pc_x86_64/audio/pipewire-hal/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/pipewire_primary_audio_policy_configuration.xml
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
