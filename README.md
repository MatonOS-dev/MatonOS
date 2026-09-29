# MatonOS

An AOSP-based operating system for generic x86_64 UEFI PCs, running a
mainline Linux kernel and Mesa graphics.

(android → Automaton → maton → MatonOS)

## Disclaimer

**This is a hobby project.** I'm building it because it's something I
wanted: Android apps on ordinary PCs, on a mainline kernel with proper
open-source graphics drivers. There is no company behind it, no support
guarantee and no schedule.

**This project heavily uses AI.** Much of the code, build scripts,
configuration and documentation was written with an AI coding assistant
(Anthropic's Claude), under my direction and with my review and testing.
Expect the kinds of mistakes that come with that, and please report them.

**Use at your own risk.** This software is provided as is, without warranty
of any kind. Installer tools in this project **erase entire disks**. Double
check the target disk, keep backups, and try the live image before
installing anything.

MatonOS is **not affiliated with, endorsed by or certified by Google**.
Android is a trademark of Google LLC; MatonOS is *based on* the Android Open
Source Project (AOSP). It ships without Google apps or services.

## Status

Early bring-up. See the roadmap and bring-up notes in [NOTES.md](NOTES.md).

- **v1**: live image that boots to the UI on generic PCs
- **v2**: installer app, display settings app
- **v3**: over-the-air updates
- **v4**: optional Google Play via add-ons, Linux app sandbox, and more

## Building: host requirements

- Linux x86_64 build host with the AOSP tree; builds run through
  `tools/build.sh` (see `HANDOFF.md`).
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
