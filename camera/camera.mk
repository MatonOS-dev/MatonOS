# Stock AOSP AIDL External Camera Provider for USB/UVC cameras.
# AOSP supplies the init rc; it has no VINTF fragment, so the manifest entry is
# ours (BoardConfig.mk, since DEVICE_MANIFEST_FILE is a board variable).
PRODUCT_PACKAGES += android.hardware.camera.provider-V1-external-service


PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/external_camera_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/external_camera_config.xml \
    frameworks/native/data/etc/android.hardware.camera.external.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.external.xml
