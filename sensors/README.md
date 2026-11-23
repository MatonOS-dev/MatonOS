# IIO accelerometer Sensors HAL

This area provides a small vendor AIDL `android.hardware.sensors.ISensors/default`
service for auto-rotation. Its AIDL FMQ/service structure follows AOSP's
Apache-2.0 example in `hardware/interfaces/sensors/aidl/default`; the HAL
implements only one sensor type, the accelerometer. It samples Linux IIO
sysfs directly and does not use buffered reads or triggers in v1.

At startup it scans `/sys/bus/iio/devices`, orders devices by index, and
prefers a device whose `name` contains “accel”. It accepts a device only when
all three `in_accel_{x,y,z}_raw` channels and their per-axis or common scale
are readable. It reads per-axis offsets or the common offset, defaulting to
zero when no offset attribute exists. It reads
`in_mount_matrix` or `in_accel_mount_matrix` when present; if neither can be
read, the transform is identity. Samples use `(raw + offset) * scale`, then
the matrix, and are reported in Android's m/s² accelerometer event format.
The service reads at the sampling period requested by the framework. The IIO
ABI defines acceleration after applying scale and offset in m/s²
([Linux IIO ABI](https://docs.kernel.org/admin-guide/abi-testing.html)).

With no usable sensor, the service still registers and returns an empty sensor
list. That is intentional for desktops and QEMU. The HAL starts asynchronously
from init and does not hold up boot. A node that disappears after discovery
logs its read failure and stops sampling while leaving the service registered.

## Android integration and policy

`config_supportAutoRotation` is explicitly true in the existing framework
overlay (AOSP's default is also true). No static
`android.hardware.sensor.accelerometer.xml` feature declaration is installed:
the same generic image may boot with no sensor, and PackageManager feature XML
is static even when the HAL returns an empty list. Claiming the feature in
that case could make app feature checks and CTS expectations inaccurate.

The service runs in the existing `matonos_driver` domain and is registered as
the stock `hal_sensors` AIDL service. AOSP's `hal_sensors_default` does not
grant generic IIO sysfs access; MatonOS's fixed driver policy already permits
read-only generic sysfs traversal. The area adds only the IIO character-device
read grant and the Sensors HAL server attribute in the existing policy file.
The dynamic `/sys/devices/**/iio:device*` nodes receive the dedicated IIO
sysfs label through the existing file-contexts policy; see
`SHARED-CHANGES.md` for the `/sys/bus/iio/devices` genfs label and product
import requested from the coordinator. `ueventd.rc` gives existing
`/dev/iio:deviceN` nodes mode `0660 system:system`.

## Verification status

No Soong, Make, lunch, or kernel build was run for this change, per task
constraints. Static review confirms AOSP Sensors AIDL V3 is the interface
version used by the service, the init executable has a domain label, and the
kernel module artifacts listed below exist in the previously built product
module bundle. A coordinator build and fresh QEMU boot are still required.

### QEMU: no sensor fallback

1. Build and boot a fresh QEMU image with no IIO device passed through.
2. Confirm `vendor.matonos-sensors` stays registered and that its log reports
   no usable IIO accelerometer.
3. Check `dumpsys sensorservice` (or the Sensors service list) shows no
   accelerometer and the device completes boot.

### Real 2-in-1: rotation

1. Boot a fresh image on a supported 2-in-1 and confirm the IIO node has raw,
   scale/offset, and mount-matrix attributes; inspect `logcat -b all -s
   matonos-sensors` for the selected device.
2. Enable auto-rotate and verify portrait, reverse portrait, landscape, and
   reverse landscape while rotating the device.
3. Compare event vectors against the physical display axes and adjust only
   after confirming the firmware/kernel mount matrix. Repeat with the
   keyboard attached/detached if the model supports tablet-mode switching.
4. Verify the service stays alive and the framework remains usable if the
   sensor is absent or disconnected.

## Kernel module availability and open work

The existing product module bundle includes the required HID sensor hub,
3D accelerometer and shared IIO modules; Intel ISH modules (`intel-ishtp*`),
Surface HID/Aggregator modules, BMC150 accelerometer, BMI160 and KXCJK1013
modules; and `modules.alias`, `modules.dep`, and `modules.load`. The requested
`intel-ish-hid` functionality is named `intel-ishtp-hid.ko` in this kernel.
No kernel configuration or module changes were needed. Runtime probe coverage
and axis orientation still need testing on actual Surface Pro 3 and newer
2-in-1 hardware. The AIDL implementation itself still needs a Soong build.
