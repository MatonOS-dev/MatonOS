#!/usr/bin/env python3
"""Host-only synthetic sysfs checks. No mounts, device access or Android VM.

Run: python3 install/linuxd/tests/udev-database-test.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "UdevDatabase.c"


def mask(bits):
    words = [0] * (max(bits, default=0) // 64 + 1)
    for bit in bits:
        words[bit // 64] |= 1 << (bit % 64)
    return " ".join(f"{word:x}" for word in reversed(words))


with tempfile.TemporaryDirectory(prefix="maton-udev-test-") as temp:
    root = Path(temp)
    sys = root / "sys"
    db = root / "udev"
    for directory in ("data", "tags/seat"):
        (db / directory).mkdir(parents=True)
    for subsystem in ("input", "hidraw", "misc"):
        (sys / "class" / subsystem).mkdir(parents=True)
    # Pull key/axis numbers from the host's stable Linux UAPI instead of
    # duplicating a second classification implementation in the test.
    harness = root / "check.c"
    harness.write_text(f'''
#define MATON_SYS_ROOT "{sys}"
#define MATON_UDEV_ROOT "{db}"
#include <sys/socket.h>
static ssize_t capture_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
#define sendto capture_sendto
#include "{SOURCE}"
#include <assert.h>
static ssize_t capture_sendto(int fd, const void *packet, size_t size, int flags,
        const struct sockaddr *address, socklen_t address_size) {{
    (void)fd; (void)flags;
    const unsigned char *bytes = packet;
    uint32_t words[8];
    assert(size > 40);
    assert(!memcmp(bytes, "libudev\\0", 8));
    memcpy(words, bytes + 8, sizeof(words));
    assert(words[0] == htonl(0xfeedcafe));
    assert(words[1] == 40 && words[2] == 40 && words[3] == size - 40);
    assert(words[4] == htonl(0xc1a28470));
    assert(words[6] || words[7]);
    assert(address_size == sizeof(struct sockaddr_nl));
    assert(((const struct sockaddr_nl *)address)->nl_groups == 2);
    assert(!strcmp((const char *)bytes + 40, "ACTION=add"));
    bool found = false;
    for (size_t pos = 40; pos < size; pos += strlen((const char *)bytes + pos) + 1)
        if (!strcmp((const char *)bytes + pos, "ID_INPUT_JOYSTICK=1")) found = true;
    assert(found);
    assert(bytes[size-1] == 0);
    return (ssize_t)size;
}}
int main(int argc, char **argv) {{
    if (argc == 2 && !strcmp(argv[1], "events")) {{
        const char valid[] = "add@/devices/test\\0SUBSYSTEM=input\\0";
        const char invalid[] = "SUBSYSTEM=input";
        assert(relevant(valid, sizeof(valid)));
        assert(!relevant(invalid, sizeof(invalid)-1));
        assert(!relevant("SUBSYSTEM=block", 16));
        assert(wire_hash("input") == 0xc1a28470);
        monitor_fd = 42;
        broadcast("E:SUBSYSTEM=input\\nE:DEVPATH=/devices/test\\nE:ID_INPUT_JOYSTICK=1\\nG:seat\\n", "add", "input");
        return 0;
    }}
    refresh();
    if (argc == 2 && !strcmp(argv[1], "forget")) maton_udev_forget_pad(makedev(13,0));
    return 0;
}}
''')
    executable = root / "check"
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-pthread", str(harness), "-o", str(executable)], check=True)

    def input_node(index, keys, axes=(), rel=(), props=(), ev=(1, 3), js=False):
        parent = sys / f"devices/virtual/input/input{index}"
        (parent / "capabilities").mkdir(parents=True)
        for kind, bits in {"ev": ev, "key": keys, "abs": axes, "rel": rel}.items():
            (parent / "capabilities" / kind).write_text(mask(bits) + "\n")
        (parent / "properties").write_text(mask(props) + "\n")
        (parent / "id").mkdir()
        for attr, value in {"vendor": "045e", "product": "028e", "bustype": "0003"}.items():
            (parent / "id" / attr).write_text(value + "\n")
        for name, minor in [(f"event{index}", index)] + ([(f"js{index}", 32 + index)] if js else []):
            node = parent / name
            node.mkdir()
            (node / "dev").write_text(f"13:{minor}\n")
            (sys / "class/input" / name).symlink_to(node)
        return parent

    pad = input_node(0, [304, 305, 307, 308], [0, 1, 3, 4, 16, 17], js=True)
    input_node(1, list(range(1, 32)), ev=[1])  # full keyboard
    input_node(2, [272], rel=[0, 1], ev=[1, 2])  # relative mouse
    input_node(3, [325, 330], [0, 1], props=[0])  # touchpad
    input_node(4, [330], [0, 1], props=[1])  # touchscreen
    input_node(5, [], [0, 1, 2], ev=[3])  # accelerometer, not joystick
    input_node(6, list(range(1, 32)) + [304, 305, 58, 69, 110, 113], [3, 4])
    input_node(7, [], [3, 4])  # buttonless rudder/pedals
    input_node(8, [272], [0, 1])  # VM absolute mouse, not controller
    input_node(9, [544, 545], ev=[1])  # dpad-only controller
    input_node(10, [256, 257], rel=[8], ev=[1, 2])  # tablet pad, not controller
    hid = sys / "devices/usb1/0003:045E:028E.0001/hidraw/hidraw0"
    hid.mkdir(parents=True)
    (hid / "dev").write_text("240:0\n")
    (hid.parent.parent / "uevent").write_text("HID_ID=0003:0000045E:0000028E\n")
    (sys / "class/hidraw/hidraw0").symlink_to(hid)
    uinput = sys / "devices/virtual/misc/uinput"
    uinput.mkdir(parents=True)
    (uinput / "dev").write_text("10:223\n")
    (sys / "class/misc/uinput").symlink_to(uinput)

    # Ancestor identities must never fill in missing inputN/HID fields.
    # This also models unrelated USB hubs above a device and virtual uinput.
    for ancestor in (sys / "devices/virtual", sys / "devices/usb1"):
        (ancestor / "idVendor").write_text("ffff\n")
        (ancestor / "idProduct").write_text("eeee\n")
        (ancestor / "uevent").write_text("DEVTYPE=usb_device\n")
    hid_bt = sys / "devices/platform/bluetooth/0005:054C:09CC.10000/hidraw/hidraw1"
    hid_bt.mkdir(parents=True)
    (hid_bt / "dev").write_text("240:1\n")
    (hid_bt.parent.parent / "uevent").write_text("DRIVER=hid-generic\nHID_ID=0005:0000054C:000009CC\n")
    (sys / "class/hidraw/hidraw1").symlink_to(hid_bt)

    def refresh():
        subprocess.run([str(executable)], check=True)

    def entry(minor):
        return (db / f"data/c13:{minor}").read_text()

    refresh()
    pad_entry = entry(0)
    for value in ("ID_INPUT=1", "ID_INPUT_JOYSTICK=1", "ID_VENDOR_ID=045e", "ID_MODEL_ID=028e", "ID_BUS=usb"):
        assert f"E:{value}\n" in pad_entry, value
    assert "ID_INPUT_JOYSTICK=1" in entry(32)
    for minor, flag in {1: "KEYBOARD", 2: "MOUSE", 3: "TOUCHPAD", 4: "TOUCHSCREEN",
                        5: "ACCELEROMETER", 7: "JOYSTICK", 8: "MOUSE", 9: "JOYSTICK", 10: "TABLET_PAD"}.items():
        assert f"E:ID_INPUT_{flag}=1\n" in entry(minor), (minor, flag)
    for minor in [1, 2, 3, 4, 5, 6, 8, 10]:
        assert "ID_INPUT_JOYSTICK" not in entry(minor), minor
    assert "G:seat\nQ:seat\n" in pad_entry
    assert (db / "tags/seat/c13:0").exists()
    assert "E:ID_BUS=usb\n" in (db / "data/c240:0").read_text()
    assert "E:DEVNAME=/dev/uinput\n" in (db / "data/c10:223").read_text()
    bt_entry = (db / "data/c240:1").read_text()
    for value in ("ID_VENDOR_ID=054c", "ID_MODEL_ID=09cc", "ID_BUS=bluetooth"):
        assert f"E:{value}\n" in bt_entry, value
    assert "ID_BUS=" not in (db / "data/c10:223").read_text()
    (hid_bt.parent.parent / "uevent").write_text("DRIVER=hid-generic\n")
    (pad / "id/vendor").unlink()
    refresh()
    assert "ID_VENDOR_ID=" not in entry(0), "read identity from unrelated ancestor"
    assert "ID_VENDOR_ID=" not in (db / "data/c240:1").read_text()
    (pad / "id/vendor").write_text("045e\n")
    refresh()
    before = (db / "data/c13:0").stat().st_mtime_ns
    refresh()
    assert entry(0) == pad_entry
    assert (db / "data/c13:0").stat().st_mtime_ns == before, "unchanged snapshot rewrote DB"
    # Real hotplug is a new sysfs node; no /dev node or access permission is needed.
    input_node(11, [304, 305], [0, 1])
    refresh()
    assert "ID_INPUT_JOYSTICK=1" in entry(11)
    (pad / "capabilities/key").write_text(mask(range(1, 32)))
    (pad / "capabilities/abs").write_text("0\n")
    refresh()
    assert "ID_INPUT_KEYBOARD=1" in entry(0)
    assert "ID_INPUT_JOYSTICK" not in entry(0)
    # An incomplete scan must not delete unrelated entries.
    (sys / "class/input/event99").symlink_to(sys / "devices/missing")
    (sys / "class/input/event11").unlink()
    refresh()
    assert (db / "data/c13:11").exists()
    (sys / "class/input/event99").unlink()
    for name in ("event0", "js0"):
        (sys / "class/input" / name).unlink()
    shutil.rmtree(pad)
    refresh()
    for minor in [0, 32, 11]:
        assert not (db / f"data/c13:{minor}").exists()
        assert not (db / f"tags/seat/c13:{minor}").exists()
    subprocess.run([str(executable), "forget"], check=True)
    assert not (db / "data/c13:0").exists()
    assert not (db / "tags/seat/c13:0").exists()
    subprocess.run([str(executable), "events"], check=True)
    print("PASS: classification, all node families, identity, tags, stable/changed DB, add/remove, partial-scan safety, uevent parsing, monitor wire format")
