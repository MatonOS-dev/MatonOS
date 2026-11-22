PRODUCT_PACKAGES += matonos-sensors-service

PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/sensors/matonos-sensors.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/matonos-sensors.rc \
    device/maton/pc_x86_64/sensors/android.hardware.sensors.xml:$(TARGET_COPY_OUT_VENDOR)/etc/vintf/manifest/android.hardware.sensors.xml
