# Forks

MatonOS changes a few AOSP and third-party projects. Each is a repository in
the [MatonOS-dev](https://github.com/MatonOS-dev) GitHub organisation with our
commits on the `matonos/v1.2` branch; `manifest/maton.xml` makes `repo sync`
check them out in place of the stock projects.

| Path | Repository | Based on | What we change |
| --- | --- | --- | --- |
| `external/minigbm` | [android_external_minigbm](https://github.com/MatonOS-dev/android_external_minigbm) | fork of android-generic `14-x86` | Mesa GBM allocator/mapper, vgem software rendering, dma-heap buffers, llvmpipe tile padding |
| `external/drm_hwcomposer` | [android_external_drm_hwcomposer](https://github.com/MatonOS-dev/android_external_drm_hwcomposer) | AOSP | DRM node scan, software copy to dumb buffers, no-RTTI, errno include |
| `hardware/baylibre/audio` | [android_hardware_baylibre_audio](https://github.com/MatonOS-dev/android_hardware_baylibre_audio) | fork of BayLibre `main` | AIDL IModule/StreamDescriptor V4 compatibility |
| `external/libdrm` | [android_external_libdrm](https://github.com/MatonOS-dev/android_external_libdrm) | AOSP | 2.4.134 for wlroots |
| `external/libxkbcommon` | [android_external_libxkbcommon](https://github.com/MatonOS-dev/android_external_libxkbcommon) | AOSP | 1.8.0 for wlroots |
| `external/pixman` | [android_external_pixman](https://github.com/MatonOS-dev/android_external_pixman) | AOSP | 0.46.0 for wlroots |
| `external/wayland` | [android_external_wayland](https://github.com/MatonOS-dev/android_external_wayland) | AOSP | 1.26.0 for wlroots |
| `external/wayland-protocols` | [android_external_wayland-protocols](https://github.com/MatonOS-dev/android_external_wayland-protocols) | AOSP | 1.48 for wlroots |

### Linux apps (Flatpak on bionic)

These check out nested inside the device tree, where `linux/third_party/<name>/Android.bp`
expects them. Each branch = upstream tag + the tested release snapshot + one commit per fix
(see `linux/third_party/FORKS-PLAN.md`). The 13 unpatched components are listed there too.

| Path | Repository | Based on | What we change |
| --- | --- | --- | --- |
| `linux/third_party/flatpak/upstream` | [flatpak](https://github.com/MatonOS-dev/flatpak) | fork of flatpak/flatpak `1.14.10` | bionic build fixes |
| `linux/third_party/ostree/upstream` | [ostree](https://github.com/MatonOS-dev/ostree) | fork of ostreedev/ostree `v2024.5` | bionic build fixes |
| `linux/third_party/bubblewrap/upstream` | [bubblewrap](https://github.com/MatonOS-dev/bubblewrap) | fork of containers/bubblewrap `v0.10.0` | bionic build fixes |
| `linux/third_party/glib/upstream` | [glib](https://github.com/MatonOS-dev/glib) | fork of GNOME/glib `2.84.4` | bionic build fixes |
| `linux/third_party/gnupg/upstream` | [gnupg](https://github.com/MatonOS-dev/gnupg) | fork of gpg/gnupg `gnupg-2.5.2` | bionic build fixes |
| `linux/third_party/npth/upstream` | [npth](https://github.com/MatonOS-dev/npth) | fork of gpg/npth `npth-1.8` | bionic build fixes |
| `linux/third_party/libgpg-error/upstream` | [libgpg-error](https://github.com/MatonOS-dev/libgpg-error) | fork of gpg/libgpg-error `libgpg-error-1.51` | bionic build fixes |
| `linux/third_party/libfyaml/upstream` | [libfyaml](https://github.com/MatonOS-dev/libfyaml) | fork of pantoniou/libfyaml `v0.9.6` | bionic build fixes |
| `linux/third_party/appstream/upstream` | [appstream](https://github.com/MatonOS-dev/appstream) | fork of ximion/appstream `v1.2.0` | bionic build fixes |

Changing a fork: commit on `matonos/v1.2` in the project checkout, push to its
MatonOS-dev repository, and upstream the fix where it makes sense.

## Dependency bumps for wlroots

These forks keep AOSP's existing Soong module names and import only the
upstream releases needed by the wlroots snapshot.

| Project | AOSP base | Upstream release | Version before → after |
| --- | --- | --- | --- |
| `external/libdrm` | `12b44e2c3b5186a7e84a5f21830b077cb8587861` | libdrm 2.4.134 (`e984d448b8b17aab853369e6c203e53719f46de1`) | 2.4.124 → 2.4.134 |
| `external/libxkbcommon` | `fc67986a8d259b977f6ecc6759794796825eb679` | xkbcommon 1.8.0 (`76740e0c4583ae49675e7ba8213d31ee09aa00d2`) | 1.4.0 → 1.8.0 |
| `external/pixman` | `94a1ec36e51d40fd4fdc2b9cbe3db392b8ad53db` | pixman 0.46.0 (`466566d7f3be42bc30af543b384555466609d8f7`) | 0.44.2 → 0.46.0 |
| `external/wayland` | `591bd295dcd97362b58c4f6ee72d910761a49da9` | Wayland 1.26.0 (`87cc8a8728a923fc57938faa81ba0e74f34ecdc7`) | 1.22.0 → 1.26.0 |
| `external/wayland-protocols` | `2dabfe6879b25981e0340823eb1b6d037d21577f` | wayland-protocols 1.48 (`02e63e74a807afed95bc25a386173110afef24e3`) | 1.22 → 1.48 |

The selected Wayland release is the newest tagged release at this update and
meets wlroots' `wayland >= 1.24.0` requirement. The four other tags meet the
version floors in the pinned wlroots snapshot.

## Build-file and generated-header updates

The Android module names and the existing module definitions remain in place.

- `external/libdrm`: no `Android.bp` changes. The release's Android source
  manifests are unchanged from AOSP's checked-in lists; `libdrm`, `libdrm_*`,
  and `libkms` keep their existing module definitions. Updated the AOSP
  `METADATA` release identifier; retained `libkms`'s AOSP-only source because
  its declared module is still present although upstream removed libkms.
- `external/minigbm`: the existing MatonOS fork needed one compatibility
  include after libdrm 2.4.134 stopped providing a transitive errno declaration
  used by minigbm's Linux host build. The one-line fix is committed on its
  existing `matonos/v1.2` branch on base
  `058236dee66a474f1e87fe9e395531a2bda21718` and exported under
  `forks/external/minigbm/`.
- `external/drm_hwcomposer`: added direct `<cerrno>` includes in the four
  translation units that use `errno`, so they do not depend on libdrm's former
  transitive include. This compatibility commit is on the existing
  `matonos/v1.2` branch on base `0e6fe0670630fba69ebbd07ae3c819c294c824fb`
  and exported under `forks/external/drm_hwcomposer/`.
- `external/libxkbcommon`: added the three new core source files from 1.8.0
  (`keysym-case-mappings.c`, `scanner-utils.c`, and `utils-paths.c`) to the
  existing `libxkbcommon` source list. Regenerated AOSP's checked-in Bison
  parser/header with Bison 3.8.2, updated its generated config version, and
  refreshed `METADATA` (the old atom warning workaround is no longer needed).
- `external/pixman`: only the three version substitutions in the existing
  `pixman-version_gen` rule changed to 0.46.0; the library module and source
  list are unchanged. Updated `METADATA`.
- `external/wayland`: the existing Soong module definitions are unchanged;
  updated the checked-in generated `wayland-version.h` to 1.26.0 and carried
  forward the AOSP client observer and safe object conversion APIs. Updated
  `METADATA`.
- `external/wayland-protocols`: kept the existing extension modules and added
  the wlroots 0.21-dev stable/staging XML inputs to their source group. The
  Chromium content-type XML remains in that group because its generated output
  basename collides with the newer freedesktop content-type XML; the upstream
  XML itself is present for direct consumers. Updated `METADATA`.

## Validation

`tools/preflight.sh` passed, and the dependency-only Soong build completed
successfully: 1,235 Ninja steps built the bumped libraries and their graphics
consumers, including libdrm, minigbm/libgbm, drm_hwcomposer, libxkbcommon,
pixman, Wayland, the protocol headers, and `wayland_scanner`. That build also
validated the minigbm and drm_hwcomposer errno include fixes documented
above.

The coordinator cleared the unrelated System Bridge and missing host APK
blockers and produced a full image successfully. I booted a byte-for-byte copy
of that image in QEMU on port 5563 (the source image remained untouched). A
fresh `-g none` boot reached `sys.boot_completed=1` with SurfaceFlinger and the
Mesa/minigbm allocator and composer services registered. For display-path
verification, a second fresh boot used QEMU `-g std`: Android reported the
built-in 1280×800 display ON, SurfaceFlinger reported Mesa llvmpipe with OpenGL
ES 3.2, and both graphics composer and allocator services were registered.
The screenshot capture command stalled, so visual pixel inspection was not
available. Virgl acceleration was not exercised because the agent runner uses
headless virgl, which is known to hang; the verified VGA path uses software
rendering. This confirms the image's display/composition fallback and the
library consumers build, but does not establish accelerated virgl operation.

## Real PC smoke test

After the image and QEMU graphics check pass, write that same image to a spare
USB drive and boot it through UEFI on the Ryzen 5800X/RX 6600 PC and the HP
ProDesk 600 G1. Confirm MatonOS reaches the launcher, the display stays stable
at native resolution, and a graphics-accelerated app opens without corruption.
Repeat on the Surface Pro 3 to check the no-RX-6600 fallback still boots. Keep
the machines' existing disks untouched and capture the boot log if a graphics
check fails.
Each local branch's base-to-branch commits are exported as numbered patches in
the matching `forks/external/<project>/` directory.
