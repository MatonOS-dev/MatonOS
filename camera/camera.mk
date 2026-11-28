# Stock AOSP AIDL External Camera Provider for USB/UVC cameras.
# AOSP supplies the init rc; it has no VINTF fragment, so the manifest entry is
# ours (BoardConfig.mk, since DEVICE_MANIFEST_FILE is a board variable).
# Coordinator opt-in only after the complete provider link has succeeded.
# Both providers implement external/0; never install/start both.
MATON_PIPEWIRE_CAMERA ?= true
ifeq ($(MATON_PIPEWIRE_CAMERA),true)
PRODUCT_PACKAGES += matonos-camera-provider
else
PRODUCT_PACKAGES += android.hardware.camera.provider-V1-external-service
endif

PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/camera/external_camera_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/external_camera_config.xml \
    frameworks/native/data/etc/android.hardware.camera.external.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.external.xml
