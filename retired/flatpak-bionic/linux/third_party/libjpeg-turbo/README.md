# libjpeg-turbo bionic dependency

Pristine libjpeg-turbo 3.2.0 official release archive, SHA-256
`6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e`. Built
as a shared x86_64 bionic library with NDK r30/API 35 and CMake. SIMD, the
TurboJPEG API, tools, tests and static output are disabled. No patches were
needed. Source and build outputs are in the out-of-tree spike under
`out/matonos/flatpak-ndk/`.

Licenses: the libjpeg API uses the Independent JPEG Group license; the
separate TurboJPEG API and build/test system use a modified 3-clause BSD
license. See `upstream/LICENSE.md` and `upstream/README.ijg`. This library is
used by gdk-pixbuf's built-in JPEG loader.
