# Build dependencies

This file lists everything a machine needs to build MatonOS: the AOSP image, the kernel, the Linux subsystem app,
and the installer/live media. It was collected while setting up a fresh Debian 13 build server.
Update it whenever a build fails on a missing tool.

Paths are never hardcoded. Each machine sets them in its own `matonos.local.env` in the AOSP tree root;
see "Path configuration" below.

## Sourcing policy

Every build input is sourced in this order:
1. **Build it** from pinned sources when that is safe. Examples: the Flatpak stack, and the D-Bus broker's bionic GLib.
2. **Fetch it** from a stable upstream, pinned by sha256, when it can't reasonably be built. Examples: the Alpine minirootfs
   and apk repositories (`setup-scratch.sh`).
3. **Commit an archive** with hashes when neither works. Example: the Microsoft-signed shim (`secureboot/shim.tar.xz`, made by
   `fetch-shim.sh`).

Never copy build outputs between machines to make a build pass: they carry the other machine's paths and can hide
outputs that no script produces any more.

## Debian packages

```sh
sudo apt-get install -y \
  rsync git-core gnupg flex bison build-essential zip unzip curl zlib1g-dev \
  libc6-dev-i386 x11proto-core-dev libx11-dev lib32z1-dev libgl1-mesa-dev libxml2-utils xsltproc \
  fontconfig python3 python-is-python3 ccache ninja-build cmake pkg-config cpio xz-utils zstd lz4 \
  libarchive-tools mtools dosfstools gdisk parted efibootmgr sbsigntool systemd-ukify patchelf \
  util-linux bubblewrap libexpat1-dev libxml2-dev libffi-dev gawk openjdk-21-jdk-headless openjdk-25-jdk-headless \
  ostree python3-gi gir1.2-ostree-1.0 libglib2.0-dev libwayland-dev libdbus-1-dev \
  clang-21 lld-21 llvm-21
```

| Package(s) | Needed by |
|---|---|
| `git-core` … `xsltproc`, `fontconfig`, `python3`, `python-is-python3` | AOSP platform build (Soong/Kati) |
| `ccache` | optional compiler cache (`CCACHE_DIR`) |
| `ninja-build`, `cmake`, `pkg-config` | subsystem native deps (compositor, Xwayland stack, PipeWire) |
| `cpio`, `xz-utils`, `zstd`, `lz4`, `libarchive-tools` | kernel/ramdisk and payload packing |
| `mtools`, `dosfstools`, `gdisk`, `parted`, `efibootmgr` | ESP and disk images (make-live.sh, installer tests) |
| `sbsigntool`, `systemd-ukify` | Secure Boot: UKI build and signing (secureboot/) |
| `patchelf` | subsystem native library fixups |
| `bubblewrap` | `alpine-enter.sh`: enters the offline Alpine root for the musl Flatpak build (needs unprivileged user namespaces) |
| `libexpat1-dev`, `libxml2-dev`, `libffi-dev` | host-side `wayland-scanner` build (compositor) |
| `gawk` | libgpg-error `gen-lock-obj.sh` (Xwayland). **mawk ≥ 20250131 parses `0x28` as 0**, so the build stops with "cannot determine bionic pthread mutex size" |
| `openjdk-21-jdk-headless`, `openjdk-25-jdk-headless` | subsystem tests (`javac` 21) and the Gradle Java toolchain (JDK 25 compiler; a JRE alone fails with "does not provide JAVA_COMPILER") |
| `ostree`, `python3-gi`, `gir1.2-ostree-1.0` | subsystem packs (`linux/packs/ostree_source.py`) and Flatpak tests |
| `libglib2.0-dev`, `libwayland-dev`, `libdbus-1-dev` | host-side subsystem tests (D-Bus bus/portal tests use `gio-unix-2.0`, Wayland tests use `wayland-client`/`-server`) |
| `clang-21`, `lld-21`, `llvm-21` | freestanding Vulkan/GL forwarder ICDs (`linux/vulkan-forward/build.sh` uses `clang-21`, `llvm-readelf-21`). Debian 13 has no LLVM 21; add `deb [signed-by=…] http://apt.llvm.org/trixie/ llvm-toolchain-trixie-21 main` |

## Toolchains and sources outside apt

Each one lives wherever the machine keeps bulk data. Its location goes in `matonos.local.env`.

| Item | Version | Variable(s) |
|---|---|---|
| Android NDK | r30 | `ANDROID_NDK`, `MATON_NDK`, `MATON_ANDROID_NDK` |
| Android SDK (MatonOS) | — | `MATON_ANDROID_SDK` |
| Gradle | 9.4.1 | `MATON_GRADLE_HOME`, cache `GRADLE_USER_HOME` |
| Node.js (via nvm) | v24.21.0 | on `PATH` (React Native apps) |
| meson | 1.9.1 | on `PATH`; the launcher must find `meson.py` relative to itself, not by an absolute path |
| SPIR-V LLVM translator | LLVM 21 | `MATON_SPIRV_PREFIX` |
| Mesa source | 26.2.3 | `MESA_DIR` (Xwayland build) |
| Zink (subsystem Mesa) | pinned upstream | checkout `MATON_ZINK_MESA`, output `MATON_ZINK_OUT`; `linux/gl-maton/build-zink.sh` (not part of `--build-dependencies`) prints `MATON_ZINK_STAGE`, `MATON_ZINK_STAGE_I386` |
| Local source checkouts | — | `LOCAL_REPOS` (holds DullPGP for the Flatpak build) |
| Offline Alpine root + musl scratch | — | `MATON_MUSL_SCRATCH`, `MUSL_SCRATCH`, `EROFS_MUSL_SCRATCH`, `MUSL_ROOTFS` |
| Linux subsystem app checkout | — | `MATON_SUBSYSTEM_REPO`, output `MATON_SUBSYSTEM_OUT` |
| Firmware / microcode cache | — | `FW_CACHE` |

A fresh subsystem checkout or worktree needs `git submodule update --init` (the `contract` store-source AIDL).

Copied build directories carry absolute paths from the machine that made them (configure caches, Makefiles).
Do not copy `deps/compositor` or `deps/pipewire` between machines; rebuild them with
`build.sh --build-dependencies` in the subsystem repo.

## Keys (never in this repo)

| Key | Variable / location |
|---|---|
| Platform/APK keys, subsystem keystore (`matonos-linux-subsystem.jks` + `.pass`) | `MATON_KEY_DIR` |
| Secure Boot keys | `~/.config/matonos` (secureboot.sh `SB_KEY_DIR`) |
| AVB key `matonos-avb-dev.pem` | `~/.config/matonos/avb/`, or `MATON_AVB_KEY_PATH` |
| OTA Ed25519 signing key | `MATON_OTA_KEY` (private key stays outside the repo); matching public key is `install/ota/ota-ed25519.pub` and is installed into system_ext. Replace the bootstrap public key with the provisioned release key before shipping an OTA-capable image. |

## Path configuration

`matonos.local.env` sits in the AOSP tree root and is not committed. `tools/build.sh` and the subsystem scripts
read it. Every path above that is not at its default belongs there, for example:

```sh
D=/srv/matonos/deps
ANDROID_NDK=$D/android-ndk-r30
MATON_NDK=$ANDROID_NDK
MATON_SUBSYSTEM_REPO=/srv/matonos/repos/linux-subsystem-app
MATON_SUBSYSTEM_OUT=/srv/matonos/subsystem-out
MATON_KEY_DIR=$HOME/.matonos-build/keys
```

Memory: `tools/build.sh` runs inside a systemd scope with `MemoryHigh=20G` (sized for a 23 GB host). On larger
machines set `MATON_BUILD_MEM_HIGH` (the 46 GB build server uses `40G`); a cap far below RAM makes Soong
analysis thrash in zram.

## Fresh tree bootstrap

The native daemons and the ODM bundle use outputs that only an AOSP build produces (vendor `libbinder_ndk`,
vendor `toolbox`, host `mkfs.erofs`). On a tree with no `out/`, run once before the first image build:

```sh
tools/build.sh -m libbinder_ndk.vendor,toolbox_vendor,mkfs.erofs
```

`-m` builds skip both steps until those outputs exist. The native build uses the prebuilt `aidl` from
`prebuilts/build-tools` when `out/` has none.

`patchelf` is required for the ODM bundle: it sets `$ORIGIN/../lib64` on the bundle's binaries.
