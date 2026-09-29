# Flatpak bionic portability spike

Upstream Flatpak 1.14.10 builds as an x86_64 bionic CLI using NDK r30/API 35
and the NDK-built dependency stack under `out/matonos/flatpak-ndk/prefix`.
This is a feasibility spike; it is not integrated into the system image.

## Configuration

The NDK build uses GLib/GIO/GObject/GModule, JSON-GLib, libarchive, GPGME,
libgpg-error, libassuan, OSTree, AppStream, libxmlb, libyaml and libfyaml from
the NDK prefix. HTTPS and XML use the image's `/system/lib64/libcurl.so` and
`libxml2.so`; curl is the platform build against BoringSSL. XZ is statically
linked into libostree because AOSP's `liblzma.so` is a different library.
Flatpak is configured without systemd, system helper, SELinux module, seccomp,
documentation, xauth, and privileged mode. Its CLI uses the `FLATPAK_BWRAP`
environment override to select the test bwrap; D-Bus proxy integration is
still absent.

The AppStream library is real (not a stub). Its build disables introspection,
docs, stemming, compose, Qt, Vala and tests. A small upstream Meson options
patch gates the appstreamcli/package-data generators and tests for this
library-only build.

## Bionic patch

`patches/0001-bionic-glnx-compat.patch` adds a guarded `strdupa` compatibility
macro using GLib's stack allocator and an `IFTODT` mode conversion for bionic,
which does not provide those glibc macros. It applies cleanly to the pinned
Flatpak upstream tree. Equivalent OSTree fixes are in
`../ostree/patches/`.

Autotools `make flatpak` does not build all `BUILT_SOURCES` when a named target
is requested, so the scratch build explicitly generates `libglnx-config.h`,
`common/flatpak-variant-private.h`, and `common/flatpak-enum-types.h` first.
This is a build invocation detail, not an upstream source patch.

## Spike results

The NDK build produced a 1.4 MiB dynamically linked PIE `flatpak` executable.
Its direct dynamic dependencies are the listed NDK libraries plus bionic,
platform libcurl, platform libxml2, and zlib. No OpenSSL library is linked.

Historical initial smoke test: an early 2026-09-29 VM run used URL-only
remote-add with GPG verification disabled and confirmed the basic 26.08 runtime
shell. That run predates the NDK GnuPG engine and the signed Flathub install
verification documented in the final VM results below.

## Open items

- Product/image integration and a system bridge for settings/control remain
  outside this portability spike.
- D-Bus proxy behavior and graphical apps/compositor integration remain
  untested.
- The current work remains an NDK portability spike, not Soong/image
  integration.

## Bionic consumer-only build (patch 0002)

`0002-bionic-prefix-host-paths.patch` is gated to the NDK `linux-android`
target. On bionic, Flatpak omits icon validation and build/export support.
The store app will decode icons with Android `BitmapFactory` when it creates
stubs. The CLI build commands removed are `build-init`, `build`,
`build-finish`, `build-export`, `build-bundle`, `build-import-bundle`,
`build-sign`, `build-update-repo`, and `build-commit-from`. The matching
sources are excluded from the bionic build; `install --bundle` and consumer
commands remain. This also keeps `flatpak-validate-icon` out of the image.

Bionic skips Flatpak's host OS libc exports and its glibc `ldconfig` /
`ld.so.cache` path. Other Linux builds keep these upstream behaviors.
`flatpak-builder` is a separate project and is not included in this Flatpak
source build.

The `gdk-pixbuf`, `libpng`, and `libjpeg-turbo` third-party source directories
remain in `linux/third_party/` but are unused by this bionic Flatpak build.
They are retained for possible later use by the store app. The old stray
`libksba 1.6.7` and `npth 1.8` directories contained only empty scaffolding;
the canonical `libksba/` and `npth/` trees remain.

The NDK r30/API 35 build passed `make -j2 all` and `make install`. The staged
stripped bundle is `out/pc-logs/flatpak-spike/ndk-runtime-gpg-consumer/`.
Compared with the prior GDK-enabled bundle, it saves 1,261,952 bytes total:
1,156,816 bytes from the removed icon validator and gdk-pixbuf/PNG/JPEG
libraries, plus 105,136 bytes from the smaller stripped `flatpak` executable
after removing build-only commands. Bundle totals are 27,506,072 bytes before
and 26,244,120 bytes after. `tools/preflight.sh` passes and both Flatpak
patches apply to the pinned upstream tree.

### VM verification

On a copy of the 2026-09-29 18:14:53 OK image (QEMU `-g none -m 4096`, port
5560, SELinux permissive), the no-codecs bundle passed `flatpak --version`
(1.14.10), imported signed Flathub, and installed
`org.freedesktop.Platform//26.08` and `io.github.zyedidia.micro` with GPG
verification enabled. The Platform shell returned 0 and reported kernel
`7.2.7-dirty`; `micro --version` returned 0 and printed 2.0.15. Flatpak on
Android needs a writable 0700 `XDG_RUNTIME_DIR` (Android has no `/run/user`).
Full command output is in `out/pc-logs/flatpak-spike/no-codecs-vm-evidence.txt`.

UID 10999 (Android app UID range) ran `flatpak --version` successfully with
the no-codecs binary and test runner. This unprovisioned UID has no
`android.permission.INTERNET` grant or Android `inet` supplementary group:
Flathub `remote-add` failed with curl error `[6] Could not resolve hostname`.
Flatpak also rejects `--user install` when invoked as root (`Refusing to
operate on a user installation as root!`). A real store app must have the
Internet permission and invoke Flatpak in its own UID to create/install into
its owned directory. The patched-bundle Platform shell and Micro launch under
UID 10999 are still pending a VM run with the app UID provisioned for network
access; the previous GDK-enabled bundle passed those two non-root launch
checks on an earlier image.

## Open items

- Product/image integration and a system bridge for settings/control remain
  outside this portability spike.
- D-Bus proxy behavior and graphical apps/compositor integration remain
  untested.
- The current work remains an NDK portability spike, not Soong/image
  integration.
