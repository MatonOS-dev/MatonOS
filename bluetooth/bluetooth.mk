LOCAL_PATH := device/maton/pc_x86_64

# Use AOSP's generic Linux HCI HAL. It uses the management channel to unblock
# rfkill and bind the kernel controller through the HCI user channel. With no
# controller, its initialization failure lets the Bluetooth stack stay off.
PRODUCT_PACKAGES += android.hardware.bluetooth-service.default
