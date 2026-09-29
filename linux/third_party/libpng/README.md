# libpng bionic dependency

Pristine libpng 1.6.58 release archive, SHA-256
`28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775`. Built as
a shared x86_64 bionic library with NDK r30/API 35, CMake, and the NDK zlib.
Tests, tools, static output and hardware-specific optimizations are disabled.
No patches were needed. Source and build outputs are tracked in the spike at
`out/matonos/flatpak-ndk/`; this is not an AOSP Soong module.

License: the upstream libpng license (permissive; see `upstream/LICENSE`).
This library is used by gdk-pixbuf's built-in PNG loader. Its stripped runtime
size will be recorded with the refreshed Flatpak bundle after VM testing.
