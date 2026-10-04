# Linux app host feasibility spike

This is an early bionic/Soong spike for the Flatpak host stack. The r24 design
ships the whole stack as one updatable APEX, `com.matonos.flatpak`, preinstalled
on `system_ext` and mounted at `/apex/com.matonos.flatpak` (see
`flatpak/APEX.md`); linuxd stays in `system_ext` and links platform libraries
from AOSP. Historical paths below that say `/system_ext/bin` or
`/system_ext/lib64` predate the APEX move.

## Source pins

| Component | Pin | Source | SHA-256 of downloaded release archive |
|---|---|---|---|
| bubblewrap | 0.13.0 | `containers/bubblewrap` release | `4734237473c0e5d695e4e9034a34e43b2dbf5164655bd13fa59ae376b2b7a765` |
| libseccomp | 2.5.5 | `seccomp/libseccomp` release | `248a2c8a4d9b9858aa6baf52712c34afefcf9c9e94b76dce02c1c9aa25fb3375` |
| GLib | 2.80.5 | GNOME release | `9f23a9de803c695bbfde7e37d6626b18b9a83869689dd79019bf3ae66c3e6771` |
| JSON-GLib | 1.8.0 | GNOME release | `97ef5eb92ca811039ad50faef59f3ae914831cff866a23f0bf9d66cfdd0fea29` |
| libarchive | 3.7.7 | upstream release | `879acd83c3399c7caaee73fe5f7418e06087ab2aaf40af3e99b9e29beb29faee` |
| libostree | 2024.5 | upstream release | `bc12d8493db64152093ee5be77cf62a29cc67a4a9e430dc987103e78aada4a6f` |
| Flatpak | 1.14.10 | upstream release | `6bbdc7908127350ad85a4a47d70292ca2f4c46e977b32b1fd231c2a719d821cd` |
| GPGME | 1.23.2 | GnuPG release | `9499e8b1f33cccb6815527a1bc16049d35a6198a6c5fae0185f2bd561bce5224` |
| libgpg-error | 1.50 | GnuPG release | `69405349e0a633e444a28c5b35ce8f14484684518a508dc48a089992fe93e20a` |
| libassuan | 3.0.1 | GnuPG release | `c8f0f42e6103dea4b1a6a483cb556654e97302c7465308f58363778f95f194b1` |
| GnuPG | 2.5.2 | GnuPG release | `7f404ccc6a58493fedc15faef59f3ae914831cff866a23f0bf9d66cfdd0fea29` |

Pinned release files live in each component's `upstream/` directory. Local
generated configuration headers live beside that directory, while Android
portability changes are separate patches under `patches/`. Run
`linux/third_party/apply-patches.sh` before a build; it checks both forward and
reverse application so rerunning it is safe. Current Soong modules are
`bwrap`, `libseccomp_matonos`, GLib core, GModule, and a staged GObject module.
GIO, JSON-GLib, libarchive, OSTree, GPGME, and Flatpak still need module
steps. AOSP provides curl, BoringSSL, libxml2, zstd, Brotli, zlib, libfuse,
libc++, libcap, and libffi.

## Current test state

The module-only build completed at 2026-09-29 00:41. It produced:

- `out/target/product/pc_x86_64/system_ext/bin/bwrap`: 101,472 bytes stripped
  (171,024 bytes unstripped). (r24: inside the APEX at
  `/apex/com.matonos.flatpak/bin/bwrap`.)
- `out/target/product/pc_x86_64/system_ext/lib64/libseccomp_matonos.so`:
  134,480 bytes stripped (226,024 bytes unstripped). (r24: inside the APEX at
  `/apex/com.matonos.flatpak/lib64/libseccomp_matonos.so`.)

The first bwrap VM smoke used the last OK image and pushed both files to
`/data/local/tmp`; boot completed as root with SELinux permissive. `bwrap
--version` reported `bubblewrap 0.10.0`. The requested command
`bwrap --unshare-all --ro-bind / / true` returned 1 with:

```
bwrap: Can't bind mount /oldroot/ on /newroot/: realpath(destination): No such file or directory
```

A second boot with rebuilt `bwrap` and patch 0003 let us isolate namespace support. As root in SELinux permissive,
Toybox `unshare` returned 0 for `-U -r`, `-m`, `-n`, `-u`, `-i`, `-p -f`,
and `-a -f`; a tmpfs mount inside a mount namespace also succeeded. The
mount test was explicitly unmounted afterward. This VM therefore permits the
user, mount, network, UTS, IPC, and PID namespace operations tested as root.

Patch `0003-bionic-root-bind-realpath.patch` falls back to the canonical
internal destination only for `ENOENT` on `/newroot/`, then lets `open()`
validate it. The rebuilt binary passes the exact requested
`bwrap --unshare-all --ro-bind / / true` command with exit status 0. Tests used
the last OK image and adb push; the module-only build did not create a fresh
image. A fresh VM booted from our saved 04:29 image copy on adb port 5564;
`sys.boot_completed` was `1`, SELinux was permissive, and after pushing the
two binaries, `bwrap --version` returned `bubblewrap 0.10.0` and the exact
requested `bwrap --unshare-all --ro-bind / / true` command exited 0. The VM
was shut down. No Flatpak binary exists yet.

The GLib core module built successfully as a 1,401,224-byte stripped shared
library. Compile fixes supplied the checked-in generated fixed-width and
integer-limit definitions; forced config ordering; `GETTEXT_PACKAGE`,
`GLIB_INTERFACE_AGE=5`, and `GLIB_BINARY_AGE=8005`; Bionic's
`HAVE_POSIX_MEMALIGN`; an empty shim for Bionic's missing `libintl.h`; system
printf selection; and a rename of GLib's private `bool` field to avoid the
C23 keyword.

The GModule module built successfully as a 51,448-byte stripped shared
library. It uses checked-in generated visibility/config headers and Bionic's
`dlopen`/`libdl` support. GObject and its generated enum/visibility headers
are staged, with AOSP's `libffi` as a dependency, but no GObject compiler
result is available yet. The coordinator stopped its module-only build during
Soong bootstrap because the user's VM on port 5555 needed memory (status
reported 0 GB free). The auto-builder is paused while the user tests; the
GObject request is still queued and must not be re-requested. Previous Soong
graph generations took up to 10m57s under memory pressure.

## Next work / open questions

1. After the coordinator resumes its auto-builder, inspect the pending
   `libgobject-2.0_matonos` module result. Then stage/build GIO and its
   dependencies, followed by OSTree and Flatpak, as module-only steps.
2. Push the built binaries to a fresh boot from an image copy and test
   `flatpak --version`, remote-add/install, and `flatpak run` in
   `/data/local/tmp`.
3. Measure GPG verification's actual runtime dependencies before deciding
   whether the `gpg` executable is needed.

The coordinator added a user constraint during this attempt: do not spawn
host commands from Flatpak's session helper (disable that helper behavior),
and never expose `org.freedesktop.Flatpak` on D-Bus. Keep any future D-Bus
surface default-deny and limited to the spike.

There are no kernel changes or shared-file changes in this spike so far.

## NDK Flatpak spike update (2026-09-29)

The NDK r30/API 35 x86_64 builds for GLib/GModule/GObject/GIO, JSON-GLib,
libarchive, libgpg-error, libassuan, GPGME, XZ Utils, and OSTree now compile
in `out/matonos/flatpak-ndk/`. OSTree 2024.5 builds both `libostree-1.so` and
`ostree`; its binary statically includes XZ Utils to avoid AOSP's incompatible
`liblzma.so`, uses `--with-crypto=glib`, and links to the image's platform
`libcurl.so` without a direct OpenSSL dependency. OSTree portability patches
and evidence are recorded in `third_party/ostree/README.md`.

Flatpak 1.14.10 configure succeeds with scratch-only marker pkg-config files
for optional dependencies, but compilation stops at the upstream hard include
`appstream.h` in `app/flatpak-builtins-utils.h`. Its remote-info, remote-ls,
and search built-ins call AppStream APIs. The requested spike excludes
AppStream, so a feature-disabled Flatpak portability patch is the remaining
work before a real `flatpak --version` VM check. The 09:05 image is available,
but no VM should start while build status reports Soong analysis in progress.

### Flatpak/AppStream progress correction (2026-09-29)

The preceding paragraph is superseded by the user's decision to build real
AppStream. NDK r30/API 35 builds now include libyaml, libfyaml, libxmlb,
AppStream 1.2.0, and Flatpak 1.14.10. The Flatpak CLI builds and was pushed
with its NDK shared libraries to a VM booted from a copy of the available
10:03 image; `flatpak --version` printed `Flatpak 1.14.10`. The requested 09:05
image had been overwritten by the later full build. See
`third_party/flatpak/README.md` for commands, exact runtime evidence, and
remaining sandbox/install tests. No Soong port has been attempted yet.

The sandbox test is now complete. On a copy of the 2026-09-29 12:18 image,
as root with SELinux permissive, URL-only Flathub remote-add and installation
of `org.freedesktop.Platform//26.08` succeeded with GPG verification disabled.
After pushing the NDK-built bwrap fix for `/newroot/usr`, this command returned
0 with empty stderr and ran inside the Flatpak sandbox:

```sh
flatpak run --command=sh org.freedesktop.Platform//26.08 \
  -c 'uname -a; ls /usr; id'
```

The `.flatpakrepo` form still fails because GPGME has no configured crypto
engine (`GPGME: Invalid crypto engine`); build/provide GnuPG before enabling
verified Flathub remotes. A separate CLI application was not installed.

## r24 security dependency rebuild

Current: bubblewrap 0.13.0, Flatpak 1.18.4 with seccomp enabled, and
xdg-dbus-proxy 0.1.9. Exact upstream commits/archive and fork source-tree
hashes are in `third_party/source-pins.json`. Fork changes remain uncommitted
as requested; see `third_party/FORKS-PLAN.md` before reproducing the build.
Earlier runtime evidence below/above is historical and does not validate r24.
