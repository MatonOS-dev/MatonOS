#!/vendor/bin/sh
# Declare Bluetooth unavailable when no controller exists at boot.
#
# pc_core_hardware.xml declares android.hardware.bluetooth. With no controller the
# HAL never sees an HCI index, the stack aborts after its HAL start timeout
# (hci_hal_impl_android.cc) and crash-loops each time Bluetooth is enabled.
# SystemConfig reads /oem/etc/permissions and drops <unavailable-feature>s, and
# SystemServer then skips the Bluetooth service, so no tile or toggle appears.
#
# Runs blocking in post-fs-data, before zygote-start. USB, PCIe and ACPI UART
# controllers register hciN when ueventd's modalias load probes them (coldboot),
# which is before this; firmware loads later, on first open. A controller added
# after boot needs a reboot.

# init starts services with umask 077; system_server (uid system) must read the file.
umask 022

for hci in /sys/class/bluetooth/hci*; do
    if [ -L "$hci" ]; then
        echo "matonos-bt-detect: controller ${hci##*/} present, Bluetooth stays declared"
        exit 0
    fi
done

mkdir -p /oem/etc/permissions || exit 1
printf '%s\n' \
    '<?xml version="1.0" encoding="utf-8"?>' \
    '<!-- Written at boot by matonos-bt-detect.sh: no Bluetooth controller. -->' \
    '<permissions>' \
    '    <unavailable-feature name="android.hardware.bluetooth" />' \
    '    <unavailable-feature name="android.hardware.bluetooth_le" />' \
    '</permissions>' > /oem/etc/permissions/matonos-no-bluetooth.xml || exit 1
echo "matonos-bt-detect: no Bluetooth controller at boot, Bluetooth features declared unavailable"
