#!/usr/bin/env python3
"""Extract the preinstalled host's executable from its signed APK."""
import pathlib
import struct
import sys
import zipfile


def extract(apk, output):
    with zipfile.ZipFile(apk) as archive:
        data = archive.read("lib/x86_64/libmatonos-dbus-broker.so")
    # This entry is an x86_64 PIE, not a JNI DSO. Reject missing/wrong payloads.
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (3, 62):
        raise ValueError("APK broker must be an x86_64 PIE ELF")
    if b"/system/bin/linker64\0" not in data:
        raise ValueError("APK broker is missing the Android ELF interpreter")
    path = pathlib.Path(output)
    path.write_bytes(data)
    path.chmod(0o755)


if __name__ == "__main__":
    extract(*sys.argv[1:])
