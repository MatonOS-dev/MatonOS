# Shared changes requested for Sensors

These are the two wiring changes blocked by the area ownership rules. Apply
them in the MatonOS device tree before building this area:

1. In `device.mk`, add the Sensors area beside the other area imports:

   ```make
   $(call inherit-product-if-exists, $(LOCAL_PATH)/sensors/sensors.mk)
   ```

2. In `sepolicy/vendor/genfs_contexts`, add the stable IIO bus directory
   label (the type is already declared in the fixed
   `sepolicy/matonos/matonos_driver.te` policy file):

   ```te
   genfscon sysfs /bus/iio/devices u:object_r:sysfs_iio_devices:s0
   ```

The per-device paths under `/sys/devices/**/iio:device*` are labeled by the
regex in the existing `sepolicy/matonos/file_contexts`. The HAL only opens a
standard `/dev/iio:deviceN` node if present; `ueventd.rc` already grants the
HAL's `system` user read access. The executable label is also present in this
worktree.
