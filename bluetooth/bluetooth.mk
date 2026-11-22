LOCAL_PATH := device/maton/pc_x86_64

# Use AOSP's generic Linux HCI HAL. It uses the management channel to unblock
# rfkill and bind the kernel controller through the HCI user channel. It needs
# the bluetooth and rfkill modules loaded at boot (matonos-bluetooth.rc); with
# them it waits for a controller instead of failing initialization, which the
# stack treats as fatal.
PRODUCT_PACKAGES += android.hardware.bluetooth-service.default
