# Ship BayLibre's generic AIDL HAL instead of the stock example audio APEX.
# The upstream checkout calls its APEX com.android.hardware.audio.generic;
# retain that actual Soong module name until a MatonOS fork can rename it.
PRODUCT_PACKAGES := $(filter-out com.android.hardware.audio,$(PRODUCT_PACKAGES))
PRODUCT_PACKAGES += com.android.hardware.audio.generic

# Keep ODM and vendor fallback policy copies aligned with the HAL's modules.

# Keep the vendor fallback policy aligned with the ODM policy. Bluetooth audio
# is disabled; do not copy the AOSP A2DP/LE Audio module configuration.
PRODUCT_COPY_FILES := $(filter-out \
    frameworks/av/services/audiopolicy/config/audio_policy_configuration_generic.xml:% \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:% \
    ,$(PRODUCT_COPY_FILES))
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config-odm/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml
# /vendor/etc/r_submix_audio_policy_configuration.xml stays AOSP's Soong module
# (aosp_r_submix_audio_policy_configuration.xml, added by an inherited .mk, so
# a filter-out here can't drop it); the ODM copy is the one policy reads.
PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/stub_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/stub_audio_policy_configuration.xml
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/audio/config-odm/mixer_controls.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_controls.xml

# The AudioTrack smoke-test app is a Gradle project in audio/testapp and is
# installed with adb for QEMU checks; it is not an image product package.
