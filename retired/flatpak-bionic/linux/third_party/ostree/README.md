# libostree NDK portability spike

Upstream source is libostree/OSTree 2024.5 in `upstream/`. The NDK r30 x86_64 API 35 build lives outside the device tree at `out/matonos/flatpak-ndk/ostree/` and installs into `out/matonos/flatpak-ndk/prefix/`.

The NDK build completes both `libostree-1.so` and the `ostree` CLI using GLib crypto (`--with-crypto=glib`), platform `libcurl` from the image, GPGME, libarchive and zlib. XZ Utils is linked statically because the platform's `liblzma.so` is the incompatible 7-Zip SDK interface. The CLI and library are x86_64 Android ELF binaries. This is a scratch NDK result; Soong integration and VM execution remain pending.

Patches under `patches/`:

- `0001-bionic-strdupa.patch`: provide the stack-copy helper absent from bionic.
- `0002-bionic-iftodt.patch`: translate `st_mode` to Android `dirent` type constants.
- `0003-bionic-version-string-compare.patch`: replace glibc `strverscmp` with a numeric-run comparison for bootloader versions.
- `0004-bionic-endian-conversion.patch`: include Android's `sys/endian.h` for `le64toh`.

All four patches passed `git apply --check` against pristine upstream and were exercised together in the successful NDK build. Configure/build/install logs are under `out/matonos/flatpak-ndk/ostree/`.

## Open work

Flatpak 1.14.10's upstream configure script hard-requires AppStream and its CLI utility headers import AppStream types. The requested minimal build excludes AppStream, so Flatpak stops at `appstream.h` during compilation after satisfying configure with a scratch-only marker `.pc`. A small feature-disabled upstream patch is needed to make the metadata listing/search helpers unavailable while keeping the package manager CLI buildable. No AppStream library or stub has been installed into the device image. Resume after the current Soong build ends; do not run a VM during Soong analysis.
