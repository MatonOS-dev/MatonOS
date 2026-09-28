# MatonOS

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

## Status

Work in progress, not yet ready for daily use.

- **Boots on real PCs and in QEMU.** The live image runs from USB or network
  boot, with Mesa (Intel, AMD, NVIDIA via nouveau/NVK, virtio), a software
  fallback for machines without a supported GPU, and Launcher3 desktop mode.
- **Hardware glue.** Our own daemons handle Wi-Fi, Bluetooth, audio card
  selection, sleep/wake (including forwarding Android wake locks) and input.
  QEMU's absolute mouse works. Firmware includes linux-firmware, SOF audio
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

The full roadmap (v2 to v7) and every design decision, with the reasons
behind it, are in
[`device/maton/pc_x86_64/NOTES.md`](device/maton/pc_x86_64/NOTES.md).

## Layout (mirrors AOSP checkout paths)

| Path | What |
|---|---|
| `device/maton/pc_x86_64/` | Device tree: product config, the ODM driver bundle, daemons, sepolicy, the system bridge, the installer service, add-ons, Secure Boot, the Linux-apps stack (`linux/`), and build/test tools (`tools/`). Start with its `README.md`, `CLAUDE.md` (rules) and `NOTES.md` (decisions). |
| `apps/settings/` | MatonOS Settings (Expo SDK 57, Expo UI Jetpack Compose only), which also contains the installer. |
| `apps/rn-common/` | Shared React Native library. It moves back out of Settings once a second app needs it (the Flatpak store). |
| `apps/launchme`, `apps/shelf`, `apps/recents` | The earlier custom shell. It is parked, not built into the image; Launcher3 replaced it. |
| `forks/<aosp path>/` | Our commits on forked AOSP projects (minigbm, drm_hwcomposer, libdrm, libxkbcommon, pixman, wayland, wayland-protocols, …) as `git format-patch` series plus `BASE`, applied on local `matonos/v1.2` branches. |

## Building

1. Sync AOSP (`android17-release`), then copy or symlink `device/maton`
   and `apps/` into the tree.
2. Run `cp device/maton/pc_x86_64/manifest/maton.xml .repo/local_manifests/`.
   Create the `matonos/v1.2` branches from `forks/*/BASE`, `git am` the
   patches, then `repo sync`.
3. Put machine-specific paths (NDK, SDK, Node) in the git-ignored
   `device/maton/pc_x86_64/matonos.local.env`. See `tools/local-env.sh`.
4. Build with `device/maton/pc_x86_64/tools/build.sh` (see the device
   README). A 32 GiB zram swap is expected. Test with
   `tools/run-qemu-live.sh`.

Build outputs, downloaded APKs, kernel/Mesa prebuilts and all signing keys
are not in this repo. The tools rebuild or fetch them.

## Downloads and updates

OS updates, driver add-ons and the MatonOS F-Droid repo are served from
<https://download.hanro50.net.za/matonos>:

- the F-Droid repo is at `fdroid/repo`;
- updates are under `updates/<version>/`, with that version's add-ons in
  `updates/<version>/addons/`.
