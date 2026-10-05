# OEM controller group, shared with ueventd and the runtime permission.
[AID_VENDOR_GAME_CONTROLLERS]
value: 2900

# Never inherited by Linux payloads; linuxd creates session pads.
[AID_VENDOR_LINUXD_UINPUT]
value: 2901

# Reserved Flatpak installer identity (system uid range, inet group). Used by
# matonos-glibc-enter to run management transactions as a non-root system uid
# with network access (group 3003 set in the entry helper). Falls within the
# OEM-reserved range so it appears in vendor/etc/passwd and vendor/etc/group.
# Not in app range, so is_system_uid returns true and netd BPF allows internet.
[AID_VENDOR_MATONOS_FPINSTALL]
value: 2902
