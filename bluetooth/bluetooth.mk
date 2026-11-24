LOCAL_PATH := device/maton/pc_x86_64

# Use the forked generic Linux HCI HAL. It waits cancellably for a controller
# on the management channel and completes initialization off the Binder thread.
PRODUCT_PACKAGES += android.hardware.bluetooth-service.matonos
