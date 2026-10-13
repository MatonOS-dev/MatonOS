# OEM controller group, shared with ueventd and the runtime permission.
[AID_VENDOR_GAME_CONTROLLERS]
value: 2900

# Reserved Flatpak installer identity (system uid range, inet group). Used by
# matonos-glibc-enter to run management transactions as a non-root system uid
# with network access (group 3003 set in the entry helper). Falls within the
# OEM-reserved range so it appears in vendor/etc/passwd and vendor/etc/group.
# Not in app range, so is_system_uid returns true and netd BPF allows internet.
[AID_VENDOR_MATONOS_FPINSTALL]
value: 2902

# Linux layer v3: the stub fork+execs this loader from the host's
# nativeLibraryDir (system APKs get no PM extraction, so the image copy needs
# the exec bit; PM-extracted updates are 0755 already).
[system_ext/priv-app/MatonWaylandHost/lib/x86_64/libmatonos-v3-loader.so]
mode: 0755
user: AID_ROOT
group: AID_ROOT
caps: 0
