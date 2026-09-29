# gdk-pixbuf NDK portability spike

Pinned GNOME gdk-pixbuf 2.42.12 (official source archive SHA-256
`b9505b3445b9a7e48ced34760c3bcb73e966df3ac94c95a148cb669ab748e3c7`). Built
for x86_64 bionic with NDK r30/API 35 using Meson and the existing GLib prefix.
The upstream tree is pristine; no compatibility patches were needed.

Configuration builds only the PNG and JPEG loaders and compiles both into
`libgdk_pixbuf-2.0.so`; GIF, TIFF and other loaders are disabled. Introspection,
documentation, man pages, tests and installed tests are disabled. Cross builds
skip the thumbnailer and runtime loader modules, so there is no loader module
directory or loader-cache step.

Dependencies built from pristine upstream sources alongside it:

- libpng 1.6.58, official SHA-256
  `28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775`;
  depends on the NDK's system zlib. License: libpng license.
- libjpeg-turbo 3.2.0, official release SHA-256
  `6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e`.
  License: IJG license for libjpeg API plus modified 3-clause BSD for the
  TurboJPEG API/build system. SIMD, tools, tests and static library are off.

The source archives and NDK build outputs are under `out/matonos/flatpak-ndk/`;
they are not Soong modules. Flatpak's full `make all` and `make install` passed
with the new `gdk-pixbuf-2.0` pkg-config dependency. The icon-validator and complete Flatpak target set linked successfully. Flatpak’s
revokefs target also required libfuse; this reused the current AOSP
`external/libfuse` source and Android config headers in an out-only NDK shared
library build. Device runtime evidence and stripped sizes are pending a fresh
VM boot after the current coordinator build finishes.

Reproduction flags: libjpeg-turbo uses shared-only CMake output with
`WITH_SIMD=OFF`, `WITH_TURBOJPEG=OFF`, `WITH_TOOLS=OFF`, and `WITH_TESTS=OFF`;
libpng uses shared-only CMake output with `PNG_TESTS=OFF`, `PNG_TOOLS=OFF`,
and `PNG_HARDWARE_OPTIMIZATIONS=OFF`. gdk-pixbuf Meson uses the NDK cross
file `out/matonos/flatpak-ndk/android-x86_64-api35.ini`,
`-Dbuiltin_loaders=png,jpeg`, `-Dpng=enabled`, `-Djpeg=enabled`, with GIF,
TIFF, other loaders, introspection, docs, man pages, tests and installed tests
disabled.

Flatpak's full build also requires `revokefs-fuse`. For this out-of-Soong test,
libfuse 3.16.2 was built from the already available AOSP
`external/libfuse` implementation/configuration headers and packaged alongside
the test bundle. AOSP identifies its shared library as LGPL-2.1; no separate
upstream source copy or device-tree patch was added for it.

## Runtime verification

On a copy of the 2026-09-29 15:26 OK image (QEMU adb 5559, root,
SELinux permissive), the refreshed stripped NDK bundle reported `Flatpak
1.14.10`. Flathub `.flatpakrepo` import had already succeeded with GPG
verification enabled; the signed `org.freedesktop.Platform//26.08` was present.
The final rerun of `flatpak run --command=sh org.freedesktop.Platform//26.08
-c 'uname -a; ls /usr; id'` returned 0, using kernel `7.2.7-dirty` and
showing the expected runtime `/usr` tree. The Flathub app `io.github.zyedidia.micro`
installed successfully (including its 25.08 runtime); its AppStream 128x128
PNG was found in the deployed app files.

Direct `flatpak-validate-icon 512 512 <icon.png>` succeeded and reported
`format=png`, `width=128`. Its `--sandbox` mode does not work against the
unmodified AOSP host: Flatpak constructs bwrap with `--ro-bind /usr /usr`, but
the host image has no `/usr` and its root is read-only. bwrap exits with
`Can't find source path /usr: No such file or directory`. A test-only wrapper
mapped `/usr` to the downloaded runtime tree, but the validator then failed to
exec itself in that constructed namespace (`execvp .../flatpak-validate-icon:
No such file or directory`); this is not counted as a successful sandbox test.
The required follow-up is a MatonOS Flatpak host-layout adaptation that supplies
an explicit runtime `/usr` bind and makes the validator's executable/dependency
paths visible inside bwrap. No image, Soong, or SELinux changes were made here.

Incremental stripped sizes are libgdk-pixbuf 173,496 bytes, libpng 249,928,
libjpeg-turbo 723,744: 1,147,168 bytes total. The full test bundle additionally
contains libfuse 260,272, `flatpak-validate-icon` 10,704, and `revokefs-fuse`
21,080 bytes (292,056 bytes), for 1,439,224 bytes total beyond the previously
built CLI bundle. Licenses: gdk-pixbuf LGPL-2.1-or-later; libpng libpng
license; libjpeg-turbo IJG plus modified 3-clause BSD; reused AOSP libfuse
LGPL-2.1.
