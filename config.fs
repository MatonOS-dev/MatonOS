# OEM controller group, shared with ueventd and the runtime permission.
[AID_VENDOR_GAME_CONTROLLERS]
value: 2900

# Linux layer v3: the stub fork+execs this loader from the host's
# nativeLibraryDir (system APKs get no PM extraction, so the image copy needs
# the exec bit; PM-extracted updates are 0755 already).
[system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-v3-loader.so]
mode: 0755
user: AID_ROOT
group: AID_ROOT
caps: 0

# The i386 sibling, kernel-exec'd by the 64->32 handoff (state_exec), which
# requires the exec bit.
[system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-v3-loader-i386.so]
mode: 0755
user: AID_ROOT
group: AID_ROOT
caps: 0
