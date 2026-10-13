#!/usr/bin/env python3
"""Check the stub's appended Binder calls against linuxd's AIDL contract."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
aidl = (root / "install/linuxd/aidl/org/matonos/systembridge/ILinuxd.aidl").read_text()
methods = re.findall(r"^\s*(?:String|void|android\.os\.ParcelFileDescriptor\[\])\s+(\w+)\(", aidl, re.M)
assert methods == ["call", "subscribe", "unsubscribe",
                   "launchSpec", "installSelf", "updateSelf", "prepareRuntime",
                   "downloadsRequest", "answerDownloads", "registerIdentity"], methods
stub = (root / "linux/compositor/host/app/src/main/java/org/matonos/compositor/stub/DownloadsGrant.java").read_text()
assert f"FIRST_CALL_TRANSACTION + (answer ? {methods.index('answerDownloads')} : {methods.index('downloadsRequest')})" in stub
assert 'writeInterfaceToken("org.matonos.systembridge.ILinuxd")' in stub
assert "input.writeString(packageName)" in stub
assert "input.writeString(request); input.writeBoolean(allow)" in stub
assert "output.readException()" in stub and "output.readString()" in stub
assert "allow &= foreground.getAsBoolean()" in stub
print("Downloads Binder contract check passed")
