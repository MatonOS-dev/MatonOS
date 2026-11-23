# Digitalis ARM64 app support

Digitalis' `android17` native bridge is configured as part of the base image.
`device.mk` conditionally inherits
`frameworks/libs/binary_translation/enable_arm64_to_x86_64.mk`; the board
configuration enables only the ARM64 native bridge ABI. It does not enable an
ARM32 bridge. The coordinator must add the Digitalis project to the manifest
at `frameworks/libs/binary_translation` before building the feature.

The Digitalis makefiles select `libberberis_arm64.so`, enable ARM64 ELF
execution through binfmt_misc, configure the ARM64 guest ISA mapping, and add
the native bridge support libraries. The added files are system artifacts;
they do not add modules to system_ext or vendor. The base product includes the
ART property defaults and SELinux support needed to load and execute the
bridge.

## Validation

Build and boot a fresh image after the manifest project is updated. Install an
ARM64-only NDK APK and confirm PackageManager accepts and launches it. Check
`getprop ro.dalvik.vm.native.bridge` returns `libberberis_arm64.so`,
`getprop ro.dalvik.vm.isa.arm64` returns `x86_64`,
`getprop ro.enable.native.bridge.exec` returns `1`, and
`/proc/sys/fs/binfmt_misc/arm64_exe` plus `arm64_dyn` are registered. Inspect
logcat for Digitalis/Berberis and linker errors while launching the APK.

Image size remains to be measured by the coordinator's build. Digitalis adds
guest runtime libraries, linker/runner binaries, binfmt configuration,
linker configuration, and proxy libraries to system; this tree does not set a
fixed system partition size, but its dynamic partition group is 8 GiB.
