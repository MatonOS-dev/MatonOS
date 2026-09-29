# MatonOS

(android → Automaton → maton → MatonOS)

MatonOS is a desktop operating system for generic x86_64 UEFI PCs, based on
AOSP (Android 17). It runs a mainline Linux kernel with Mesa graphics and
uses stock Launcher3 in desktop mode. It keeps the AOSP framework as close
to stock as possible: MatonOS lives in configuration, our own
daemons/HALs/apps, and one system bridge app.

> **AI disclosure.** MatonOS is developed with heavy use of AI coding
> assistants: Anthropic's Claude (Claude Code) as coordinator, plus
> OpenAI Codex agents for individual areas. A human (the project owner)
> sets direction, makes the design decisions and tests on real hardware,
> but much of the code, build tooling and documentation in this repository
> was written by AI agents and may contain mistakes. Review before you rely
> on it, especially anything touching disks, security or signing.

## Disclaimer

**This is a hobby project.** I'm building it because it's something I
wanted: Android apps on ordinary PCs, on a mainline kernel with proper
open-source graphics drivers. There is no company behind it, no support
guarantee and no schedule.

**Use at your own risk.** This software is provided as is, without warranty
of any kind. Installer tools in this project **erase entire disks**. Double
check the target disk, keep backups, and try the live image before
installing anything.

MatonOS is **not affiliated with, endorsed by or certified by Google**.
Android is a trademark of Google LLC; MatonOS is *based on* the Android Open
Source Project (AOSP). It ships without Google apps or services.

## Contributing

See [CONTRIBUTE.md](CONTRIBUTE.md). In short: human code and assets are
welcome; AI image, video and music generation is not; and you are
responsible for any code you submit, however it was produced.

## Status

Work in progress, not yet ready for daily use.

- **Boots on real PCs and in QEMU.** The live image runs from USB or network
  boot, with Mesa (Intel, AMD, NVIDIA via nouveau/NVK, virtio), a software
  fallback for machines without a supported GPU, and Launcher3 desktop mode.
- **Hardware glue.** Our own daemons handle Wi-Fi, Bluetooth, audio card
  selection, sleep/wake (including forwarding Android wake locks) and input.
  QEMU's absolute mouse tracks the host cursor.
  Firmware includes linux-firmware, SOF audio
  and the Wi-Fi regulatory database.
- **Apps.**
  - MatonOS Settings is an Expo UI app, opened from Android Settings'
    homepage.
  - F-Droid Basic is built by us as a privileged app, so installs are
    silent.
  - microG uses LineageOS-style signature spoofing.
  - Aurora Store and YouTube ship as placeholders that the real apps install
    over. Aurora is in the MatonOS F-Droid repo.
- **In progress:** an A/B installer (install from the live image to a
  disk), Secure Boot via shim with our own keys, signed driver add-ons, and
  v4 Linux app support (Flatpak on bionic plus a wlroots-based
  compositor).

The full roadmap (v2 to v8) and every design decision, with the reasons
behind it, are in
[`NOTES.md`](NOTES.md).

## Layout

This repository is the device tree; check it out at
`device/maton/pc_x86_64` inside an AOSP tree.

| Path | What |
|---|---|
| `./` | Product config, the ODM driver bundle, daemons, sepolicy, the system bridge (`systembridge/`), the installer service (`install/`), add-ons, Secure Boot, and build/test tools (`tools/`). Start with `CLAUDE.md` (rules) and `NOTES.md` (decisions). |
| `linux/` | Linux apps: the Flatpak-on-bionic stack (`third_party/`, patches over pinned upstream sources) and the Wayland compositor host. |
| `rn-apps/settings/` | MatonOS Settings (Expo SDK 57, Expo UI Jetpack Compose only), which also contains the installer. |
| `rn-apps/flathub/` | Software Centre: installs Linux apps (Flatpaks from Flathub). |
| `rn-apps/rn-common/` | Shared React Native library. |
| `rn-apps/launchme`, `rn-apps/shelf`, `rn-apps/recents` | The earlier custom shell. It is parked, not built into the image; Launcher3 replaced it. |
| `FORKS.md` | Our forks of AOSP/third-party projects (minigbm, drm_hwcomposer, BayLibre audio, libdrm, libxkbcommon, pixman, wayland, wayland-protocols) in the [MatonOS-dev](https://github.com/MatonOS-dev) org, pulled in by `manifest/maton.xml`. |

## Building

See **[BUILD.md](BUILD.md)** for the full setup: host requirements, AOSP
sync with our local manifest, machine settings, building and testing in QEMU.

Build outputs, downloaded APKs, kernel/Mesa prebuilts and all signing keys
are not in this repo. The tools rebuild or fetch them.

## Building: host requirements

- Linux x86_64 build host with the AOSP tree; builds run through
  `tools/build.sh` .
- **zram swap is required**: at least 16 GiB, at the highest swap priority
  (recommended: 32 GiB, zstd). Soong's analysis needs far more memory than a
  typical host has; zram compresses it (~6×) and keeps swapping at RAM speed.
  `tools/build.sh` checks this and prints the setup commands if it's missing:

  ```sh
  sudo apt install -y systemd-zram-generator
  printf '[zram0]\nzram-size = 32768\ncompression-algorithm = zstd\nswap-priority = 200\n' \
    | sudo tee /etc/systemd/zram-generator.conf
  sudo systemctl daemon-reload && sudo systemctl restart systemd-zram-setup@zram0.service
  ```

- A second, disk-backed swap file on an NVMe drive (e.g. 32–48 GiB, lower
  priority) is a good fallback behind zram.

## Downloads and updates

OS updates, driver add-ons and the MatonOS F-Droid repo are served from
<https://download.hanro50.net.za/matonos>:

- the F-Droid repo is at `fdroid/repo`;
- updates are under `updates/<version>/`, with that version's add-ons in
  `updates/<version>/addons/`.

## Built on

MatonOS is mostly an assembly of other people's excellent open-source work:

- [Android Open Source Project](https://source.android.com/)
- [Linux](https://kernel.org/) (mainline stable) and
  [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
- [Mesa](https://mesa3d.org/)
- [BlissOS](https://blissos.org/) / [android-generic](https://github.com/android-generic)
  minigbm, including the gbm_mesa backend originally from
  [GloDroid](https://github.com/GloDroid)
- [drm_hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer)
- [systemd-boot](https://systemd.io/BOOT/)
- [wireless-regdb](https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git)
- BayLibre's generic AIDL audio HAL
- [microG](https://microg.org/) (GmsCore, Companion, GsfProxy); the signature
  spoofing patch follows [LineageOS](https://lineageos.org/)'s approach
- [F-Droid](https://f-droid.org/) (F-Droid Basic, our own build)
- Preinstalled apps: [Fossify](https://github.com/FossifyOrg) (Calculator,
  Calendar, Clock, Contacts, Gallery, Music Player, Notes),
  [Fennec F-Droid](https://f-droid.org/packages/org.mozilla.fennec_fdroid/),
  [Open Camera](https://opencamera.org.uk/)
- [Expo](https://expo.dev/) / [React Native](https://reactnative.dev/)
  (MatonOS Settings and our other apps)
- Linux apps (in development): [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots),
  [Flatpak](https://flatpak.org/), [OSTree](https://ostreedev.github.io/ostree/),
  [bubblewrap](https://github.com/containers/bubblewrap),
  [GLib](https://gitlab.gnome.org/GNOME/glib),
  [libseccomp](https://github.com/seccomp/libseccomp),
  [GnuPG](https://gnupg.org/) and their dependencies (see
  `linux/third_party/LICENSES`)

Each component keeps its own licence; see the upstream projects.

## Licence

MatonOS's own code is licensed under the [Apache License 2.0](LICENSE),
matching AOSP. Patches to upstream projects (`patches/`) and the kernel
configuration follow their upstream licences (e.g. the Linux kernel is
GPL-2.0). Third-party components keep their own licences.
