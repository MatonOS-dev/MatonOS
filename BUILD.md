# Building MatonOS

This guide takes a fresh Linux machine to a bootable MatonOS live image.
Expect a long first build: a full AOSP sync is ~150 GB and the first image
build takes several hours.

## 1. Host requirements

- Linux x86_64 (Ubuntu/Mint/Debian tested), **≥ 16 GB RAM**, **≥ 400 GB**
  free disk (AOSP checkout + `out/`).
- **zram swap ≥ 16 GiB at the highest swap priority** (32 GiB zstd
  recommended). Soong's analysis needs far more memory than most hosts have;
  `tools/build.sh` refuses to run without it and prints the setup commands.
  A lower-priority disk swap file behind it is a good fallback.
- AOSP's host packages (see
  <https://source.android.com/docs/setup/start/requirements>) plus `repo`.
- Extra tools our scripts use: `meson` ≥ 1.4, `ninja`, `cmake`, `python3`,
  `sgdisk`, `mtools`, `dosfstools`, `erofs-utils`, `qemu-system-x86` and
  `ovmf` (for testing), `curl`, `jq`.
- **Android NDK r30** (native daemons, Mesa, PipeWire) and an **Android SDK**
  (our Gradle/Expo apps).
- **Node.js ≥ 20.19** (Expo SDK 57). Node 24 via `nvm` works.
- Sources built outside AOSP:
  - a **mainline Linux** kernel tree (stable 7.2.x) and **linux-firmware**
    (sparse-cloned on demand by `tools/build-kernel.sh`);
  - a **Mesa 26.2.x release** tarball from <https://archive.mesa3d.org/>,
    plus SPIRV-LLVM-Translator 21 if your distro lacks it.

## 2. Get AOSP and MatonOS

MatonOS's local manifest adds this device tree and our forked projects
(see [FORKS.md](FORKS.md)) on top of AOSP:

```sh
mkdir aosp && cd aosp
repo init -u https://android.googlesource.com/platform/manifest -b android-latest-release
mkdir -p .repo/local_manifests
curl -o .repo/local_manifests/maton.xml \
  https://raw.githubusercontent.com/MatonOS-dev/MatonOS/main/manifest/maton.xml
repo sync -c -j8
```

The device tree lands in `device/maton/pc_x86_64`; run the commands below
from there unless noted.

## 3. Machine settings

Create `matonos.local.env` in the **AOSP root** (git-ignored; read by
`tools/local-env.sh`) with your paths, for example:

```sh
PATH=$HOME/.nvm/versions/node/v24.21.0/bin:$PATH
ANDROID_NDK=$HOME/Documents/android-ndk-r30
MATON_ANDROID_SDK=$HOME/Documents/matonos-android-sdk
MATON_SPIRV_PREFIX=$HOME/.local/opt/spirv-llvm-21
```

`tools/build-kernel.sh` and `tools/build-mesa.sh` take the kernel and Mesa
source paths as `-s` / `-m` (or `LINUX_DIR` / `MESA_DIR`).

## 4. Fetch and build the pieces outside Soong

Run from `device/maton/pc_x86_64`:

```sh
tools/build-kernel.sh -s ~/src/linux      # kernel, modules, firmware -> prebuilt/
tools/build-mesa.sh -m ~/src/mesa-26.2.3  # Mesa -> prebuilt/mesa
tools/build-pipewire.sh                   # PipeWire stack -> prebuilt/pipewire
tools/build-native.sh                     # our C daemons -> prebuilt/
tools/build-apps.sh                       # Settings, Flathub, ... (Expo/Gradle) -> buildinfra/apps-built/
tools/fetch-apps.sh                       # pinned F-Droid APKs -> apps/apks/
gms/fetch-gms.sh                          # pinned microG APKs
linux/third_party/apply-patches.sh        # Flatpak stack patches (needs the pinned upstream sources)
```

Our apps are signed with per-app development keys that `build-apps.sh`
creates on first run (kept outside the repo). Nothing in this repository
contains signing keys.

## 5. Build the image

```sh
MATON_BUILD_COORDINATOR=1 tools/build.sh
```

`build.sh` runs the steps above that are out of date, applies the small AOSP
patches in `patches/`, builds AOSP (`pc_x86_64-userdebug`) and packs the live
image at `out/target/product/pc_x86_64/matonos-live-x86_64.img`. Useful flags:
`-K`/`-M` skip the kernel/Mesa build, `-m <modules>` builds single modules,
`-j <n>` limits jobs (use 6–8 on hosts with ≤ 32 GB RAM).

If Soong fails with "provider … modified after being set", run once with
`SOONG_INCREMENTAL_ANALYSIS=false`.

## 6. Test it

```sh
tools/run-qemu-live.sh -g std        # windowed, software rendering
tools/run-qemu-live.sh               # virgl (host GPU)
adb connect localhost:5555
```

Or write the image to a USB stick with `dd` and boot a PC from it (UEFI).
The live image keeps nothing between boots.

## Where to go next

- `README.md`: what MatonOS is and the repository layout.
- `NOTES.md`: roadmap and every design decision.
- `CLAUDE.md`: rules and known build traps.
