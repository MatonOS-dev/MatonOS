# Stock AOSP AIDL External Camera Provider for USB/UVC cameras.
# The service's own init rc and VINTF fragment are supplied by AOSP.
PRODUCT_PACKAGES += android.hardware.camera.provider-V1-external-service

DEVICE_MANIFEST_FILE += $(LOCAL_PATH)/camera/android.hardware.camera.provider-V1-external-service.xml

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/external_camera_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/external_camera_config.xml \
    frameworks/native/data/etc/android.hardware.camera.external.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.external.xml
