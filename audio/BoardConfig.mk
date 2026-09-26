# Audio policy is consolidated in the shared MatonOS driver domain.

# ueventd reads this in /vendor/lib/modules when probing HDA controllers.
BOARD_VENDOR_KERNEL_MODULES_OPTIONS_FILE := $(DEVICE_PATH)/audio/modules.options
