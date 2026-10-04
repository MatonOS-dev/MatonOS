# libgpg-error for bionic

Pinned upstream libgpg-error 1.51 from the official GnuPG release archive,
SHA-256 `be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2`.
This version is required by GnuPG 2.5.2 (`gpg` configure requires >=1.51).
The shared NDK prefix and the rebuilt libassuan, GPGME, OSTree and Flatpak
artifacts use this one version.

The single local patch adds the 40-byte x86_64 bionic pthread mutex lock ABI
description, needed for generated `gpg-error.h`. It is checked against the
pristine 1.51 release source and builds with NDK r30/API 35 for x86_64. The
build lives under `out/matonos/flatpak-ndk/`; source archive and build artifacts
are not included in the device image in this spike.

The library is LGPL-2.1-or-later. The stripped runtime copy in the refreshed
bundle is 143,584 bytes. GPG signature import, Flathub platform installation
with GPG verification enabled, and rejection of an unsigned OSTree summary
passed on a copied QEMU image; see `linux/third_party/gnupg/README.md` for
runtime evidence.
