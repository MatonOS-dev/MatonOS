# GnuPG `gpg` bionic port

This is a portability spike for the standalone OpenPGP `gpg` executable only.
The source is the upstream GnuPG 2.5.2 tree (revision `84e1781201489e50888c9415bb2625f9dd27cb8a`). Builds use NDK r30,
API 35, x86_64, with all generated files under
`out/matonos/flatpak-ndk/gnupg/`.

Configure omits S/MIME, smartcard, dirmngr, keyboxd, sqlite/TOFU, LDAP, tests,
documentation, and optional compression/network features. The agent is forced
off by a small local configure patch because upstream no longer offers an
`--disable-agent` switch. GPGME can discover the executable through `PATH`;
OSTree gives each verification context a temporary GnuPG home, while its
trusted remote keys remain in the Flatpak installation repository. The VM
recipe sets `HOME`, `GNUPGHOME` and `TMPDIR` under the Flatpak installation,
so OSTree temporary GPG homes also stay there; no app `$HOME` is used.

`libgcrypt` 1.11.1 is pinned from the official GnuPG release archive
(SHA-256 `24e91c9123a46c54e8371f3a3a2502f1198f2893fbfbf59af95bc1c21499b00e`).
`libksba` 1.6.7 and `npth` 1.8 are additional configure-required GnuPG
libraries. nPth's upstream configure checks `pthread_cancel`, which bionic
omits; the patch probes `pthread_create` instead. Upstream release archive
SHA-256 values: libksba `cf72510b8ebb4eb6693eef765749d83677a03c79291a311040a5bfd79baab763`,
npth `8bd24b4f23a3065d6e5b26e98aba9ce783ea4fd781069c1b35d149694e90ca3e`.

## Status

NDK build status: `gpg` linked successfully for x86_64 bionic and the build
produced no `gpg-agent`, dirmngr, keyboxd, or scdaemon binaries. The stripped PIE is 983,536 bytes. Its direct non-bionic dependencies are
libgcrypt, libassuan, libnpth and libgpg-error. The stripped runtime copies add
about 1.72 MiB for libgcrypt and 18 KiB for npth; libassuan is already part of
the Flatpak bundle and libgpg-error is being upgraded in that bundle. The
binary does not dynamically link libksba; GnuPG's common build requires its
headers/configure metadata for this release.

Licenses: the `gpg` executable is GPL-3.0-or-later and runs as its own process.
Runtime libraries are LGPL-2.1-or-later (libgcrypt with its upstream linking
exception, plus libassuan, npth and libgpg-error). Libksba is mixed GPL/LGPL
and is not a direct dynamic dependency of `gpg`; per-file terms are in its
upstream license files.

GnuPG 2.5.2 configure requires libgpg-error >=1.51; Flatpak's existing stack
uses 1.50. A separate 1.51 NDK build uses the existing
`libgpg-error/patches/0001-bionic-x86_64-lock-object.patch` in scratch and a
separate `out/matonos/flatpak-ndk/gpg-prefix`, avoiding changes to the shared
Flatpak prefix. Its upstream 1.51 archive SHA-256 is
`be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2`.

VM signature verification, signed Flathub install, tamper rejection and
sandbox rerun are pending.

## VM verification (2026-09-29)

On a copy of the 14:05 full-OK image, adb port 5558, root, SELinux permissive:

- `gpg` 2.5.2 was found by GPGME through `PATH`; `flatpak --version` returned
  `Flatpak 1.14.10`.
- `flatpak --system remote-add --from flathub
  https://dl.flathub.org/repo/flathub.flatpakrepo` succeeded. The installation
  repo contains `flathub.trustedkeys.gpg`; remote config shows both
  `gpg-verify=true` and `gpg-verify-summary=true`.
- `flatpak --system install --assumeyes --no-related flathub
  org.freedesktop.Platform//26.08` completed (281.7 MB), with verification
  enabled.
- `flatpak run --command=sh org.freedesktop.Platform//26.08 -c 'uname -a; ls
  /usr; id'` returned 0. Guest kernel reported Linux 7.2.7 x86_64.
- A generated OSTree repository with an unsigned summary was rejected at
  `remote-add`: “GPG verification enabled, but no summary signatures found”.
  `remote-ls` also rejected it with the same signature requirement.

GPG home, HOME and TMPDIR were all under
`/data/matonos/linux/flatpak-gpg-install`; the application user's home was not
used. The test bundle has since been refreshed against shared libgpg-error
1.51 and rebuilt libassuan/GPGME/OSTree/Flatpak outputs; see the 1.51 bundle at
`out/pc-logs/flatpak-spike/ndk-runtime-gpg-1.51/`.

Stripped GPG-specific additions to the prior Flatpak bundle: `gpg` 983,536
bytes, `libgcrypt.so` 1,715,360 bytes, and `libnpth.so` 18,336 bytes
(2,717,232 bytes combined). libassuan and libgpg-error were already part of
the Flatpak runtime. The GPG executable is GPL-3.0-or-later and runs as a
separate process; libgcrypt and npth are LGPL-2.1-or-later (libgcrypt has its
upstream linking exception). The rebuilt 1.51 `libgpg-error.so` is 143,584
bytes stripped.

Static project preflight passed after the port and the 1.51 update:
`MATON_BUILD_COORDINATOR=1 tools/preflight.sh` reported clean device modules,
prebuilts, ODM registry, SELinux file set, patches, and Android.bp formatting.
No Soong build was started for this work.
