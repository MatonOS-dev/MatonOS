#!/usr/bin/env python3
"""D1 command boundary and PackageManager callback removal regression."""
from pathlib import Path
root = Path(__file__).resolve().parents[3]
service = (root / "install/linuxd/MatonosLinuxd.cpp").read_text()
aidl = (root / "install/linuxd/aidl/org/matonos/systembridge/ILinuxd.aidl").read_text()
manager = (root / "install/linuxd/FlatpakManager.c").read_text()
app = (root / "linux/subsystem/host/app/src/main/java/org/matonos/linux/LinuxAppStoreService.java").read_text()
commands = ["begin_staged_install", "verify_staged_install", "abort_staged_install",
            "bind_pending", "install_complete", "install_failed", "uninstall",
            "reconcile_deployments", "list_installed", "installed_commit"]
for command in commands:
    assert f"\"{command}\"" in service or command in service, command
for command in ["check_update", "desktop_entry", "pending_desktop", "pending_icon",
                "pending_metadata", "deploy_pending", "abort_pending", "add_flathub",
                "install_repo", "set_stub_signer"]:
    assert f"\"{command}\"" not in service, command
    assert command not in aidl, command
for old in ["ILinuxdIdentity", "registerIdentity", "stub_identity_query", "stub_verify_caller"]:
    assert old not in service and old not in aidl, old
for old in ["pending_metadata", "pending_desktop", "pending_icon", "check_update"]:
    assert old not in app, old
assert "flatpak_manager_staged" in manager and "flatpak_manager_installed_for_uid" in manager
print("PASS D1 linuxd command surface; bridge identity callback and app-side metadata calls removed")
