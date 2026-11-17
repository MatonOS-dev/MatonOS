# Build dependencies

This file lists everything a machine needs to build MatonOS: the AOSP image, the kernel, the Linux subsystem app,
and the installer/live media. It was collected while setting up a fresh Debian 13 build server.
Update it whenever a build fails on a missing tool.

Paths are never hardcoded. Each machine sets them in its own `matonos.local.env` in the AOSP tree root;
see "Path configuration" below.

## Debian packages

```sh
sudo apt-get install -y \
  rsync git-core gnupg flex bison build-essential zip unzip curl zlib1g-dev \
  libc6-dev-i386 x11proto-core-dev libx11-dev lib32z1-dev libgl1-mesa-dev libxml2-utils xsltproc \
  fontconfig python3 python-is-python3 ccache ninja-build cmake pkg-config cpio xz-utils zstd lz4 \
  libarchive-tools mtools dosfstools gdisk parted efibootmgr sbsigntool systemd-ukify patchelf \
  util-linux bubblewrap libexpat1-dev libxml2-dev libffi-dev gawk openjdk-21-jdk-headless \
  ostree python3-gi gir1.2-ostree-1.0 libglib2.0-dev libwayland-dev libdbus-1-dev
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
| `openjdk-21-jdk-headless` | subsystem tests (`javac`) and Gradle |
| `ostree`, `python3-gi`, `gir1.2-ostree-1.0` | subsystem packs (`linux/packs/ostree_source.py`) and Flatpak tests |
| `libglib2.0-dev`, `libwayland-dev`, `libdbus-1-dev` | host-side subsystem tests (D-Bus bus/portal tests use `gio-unix-2.0`, Wayland tests use `wayland-client`/`-server`) |

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
| Zink stage (subsystem Mesa) | — | `MATON_ZINK_STAGE`, `MATON_ZINK_STAGE_I386` |
| Offline Alpine root + musl scratch | — | `MATON_MUSL_SCRATCH`, `MUSL_SCRATCH`, `EROFS_MUSL_SCRATCH`, `MUSL_ROOTFS` |
| Linux subsystem app checkout | — | `MATON_SUBSYSTEM_REPO`, output `MATON_SUBSYSTEM_OUT` |
| Firmware / microcode cache | — | `FW_CACHE` |

Copied build directories carry absolute paths from the machine that made them (configure caches, Makefiles).
Do not copy `deps/compositor` or `deps/pipewire` between machines; rebuild them with
`build.sh --build-dependencies` in the subsystem repo.

## Keys (never in this repo)

| Key | Variable / location |
|---|---|
| Platform/APK keys, subsystem keystore (`matonos-linux-subsystem.jks` + `.pass`) | `MATON_KEY_DIR` |
| Secure Boot keys | `~/.config/matonos` (secureboot.sh `SB_KEY_DIR`) |
| AVB key `matonos-avb-dev.pem` | `~/.config/matonos/avb/`, or `MATON_AVB_KEY_PATH` |

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

Some scripts still fall back to `$HOME/...` defaults when a variable is unset. Those are bugs to remove; set the
variable rather than recreating the default directory.
