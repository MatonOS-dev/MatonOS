#!/vendor/bin/sh
# pc-wakeup.sh: let keyboards and mice wake the PC from s2idle.
#
# Android suspends when the screen turns off, so a key press or mouse
# movement/click must be able to resume the system. Linux enables wakeup on
# USB keyboards itself, but leaves mice, the i8042 mouse/touchpad port and
# often the hubs and controllers between them and the CPU off. Enable:
#   - PCI USB controllers (class 0x0c03) that support it (firmware _PRW),
#   - USB root hubs and hubs,
#   - boot-protocol USB keyboards and mice (HID class 3, protocol 1/2),
#   - the i8042 keyboard and mouse/touchpad ports (PS/2, laptops).
# The power button always works. Runs once at boot (init.pc_x86_64.rc);
# devices plugged in later keep the kernel default (keyboards: on) until
# hotplug handling exists.

wake_on() {
    [ -w "$1" ] && echo enabled > "$1"
}

for d in /sys/bus/pci/devices/*; do
    case $(cat "$d/class") in
        0x0c03*) wake_on "$d/power/wakeup" ;;
    esac
done

for d in /sys/bus/usb/devices/*; do
    case ${d##*/} in
        usb*)                                   # root hub
            wake_on "$d/power/wakeup" ;;
        *:*)                                    # interface of a device
            [ "$(cat "$d/bInterfaceClass")" = 03 ] || continue
            case $(cat "$d/bInterfaceProtocol") in
                01|02) wake_on "${d%:*}/power/wakeup" ;;  # keyboard, mouse
            esac ;;
        *)                                      # device: hubs only
            [ "$(cat "$d/bDeviceClass" 2>/dev/null)" = 09 ] &&
                wake_on "$d/power/wakeup" ;;
    esac
done

for d in /sys/bus/serio/devices/serio*; do
    wake_on "$d/power/wakeup"                   # i8042 KBD and AUX ports
done

exit 0
