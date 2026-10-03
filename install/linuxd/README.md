# Flatpak linuxd

`matonos-linuxd` exposes the Flatpak manager to the system bridge through the
existing `ILinuxd` Binder interface. It accepts structured JSON only, validates
refs and app IDs before invoking the fixed `/system_ext/bin/flatpak` binary,
and runs package operations on a worker while reporting progress. It starts
after `sys.boot_completed`; a missing service or Flatpak payload does not block
boot.

## Input-device metadata for Flatpak

Linuxd starts `UdevDatabase.c` before accepting Flatpak operations. It reads
sysfs (never device nodes) and publishes `/data/matonos/linux/udev/data/cM:m`
for input event/js nodes, hidraw nodes and uinput. Input classification uses
capability bitmaps, with touch/keyboard exclusions for controller detection;
vendor/product/bus come only from the inputN IDs or hidraw's direct HID
parent uevent. No arbitrary USB/Bluetooth/PCI ancestors are probed. Hidraw
and uinput receive metadata even when device access is denied, but are not
falsely labelled as joysticks without input capabilities. There are no
application-ID exceptions or changes to device permissions.

The database uses udev's `E:` properties, `I:` initialization timestamp,
`G:` tags and `Q:` current tags. Input nodes have the `seat` tag, default
`ID_SEAT=seat0`, and corresponding empty `tags/seat/cM:m` index files. Files
are replaced atomically within a stable directory; unchanged entries retain
their timestamp and inode. A regular `control` existence marker satisfies
libudev's active-udev check; it does not implement udevadm's control protocol.

The bwrap shim identifies app sandboxes through the presence of the
`--setenv FLATPAK_ID` option (direct or bundled), binds this directory
read-only at `/run/udev`, and sets `SDL_JOYSTICK_DISABLE_UDEV=1` for every app.
Flatpak already binds `/sys/class`, `/sys/dev` and `/sys/devices` read-only;
`--device=all` binds `/dev` as a directory so future input nodes appear too.
The shim adds no device binds and grants no access beyond Flatpak and Android
permissions. The inspected NDK Flatpak checkout's `flatpak-run.c` has no
`/run/udev` bind, so the explicit shim bind is required.

A kernel `NETLINK_KOBJECT_UEVENT` group-1 listener refreshes the snapshot for
input, hidraw and misc events. A two-second reconciliation also recovers
missed events, startup/sysfs races and listener failures. Incomplete scans
preserve stale entries until a complete scan can safely remove them.
Successful database updates attempt group-2 multicast using libudev's
40-byte `libudev` header, network-order magic/subsystem/tag hashes and
NUL-separated properties. Removal events carry the previous metadata.
Multicast is best effort: linuxd runs as `system` without `CAP_NET_ADMIN`,
older libudev expects root sender credentials, and isolated network
namespaces cannot reliably receive host multicast. No extra capability or
namespace privilege is added for this.

SDL2 and SDL3 both recognize the generic disable-udev joystick hint and
use initial `/dev/input` enumeration plus inotify for create/delete/move and
permission changes; when inotify is unavailable, they poll. This also avoids
a libudev monitor which opens successfully but never delivers events. The
database remains usable by other libudev consumers for enumeration and
properties. Reliable monitor-based hotplug for those consumers remains
unverified/unsupported when multicast is blocked; this is not a full udevd
or logind replacement.

For enforcing builds, device-owned system_ext policy gives linuxd read access
only to `sysfs_matonos_input` metadata. Ueventd applies topology-independent
`file_contexts` patterns under `/sys/devices` at coldboot and hotplug. Class
directories/links use `sysfs_matonos_discovery` genfs labels because class links
are outside device uevent subtrees; this type grants no regular-file access.
Other sysfs directories allow only traversal and `getattr` for Bionic realpath.
Generic sysfs files, device-node permissions and controller gates are unchanged.

Host fixture checks: `python3 install/linuxd/tests/udev-database-test.py`.
This uses temporary synthetic sysfs and database directories, without mounts,
VMs or device access. Compile-check C sources with the NDK API-35 clang and
`-O2 -Wall -Wextra -Werror -march=x86-64-v2`, as in `tools/build-native.sh`.
Runtime verification still needs an authorized fresh image: inspect the DB
inside a `--device=all` sandbox, compare libudev enumeration with sysfs,
and plug/unplug a controller while an SDL2/SDL3 app runs. Device access and
the separate controller-permission branch must be checked independently.

Run arguments are passed after `--`, and caller-provided values beginning with
`-` are rejected. Uninstall keeps app data unless `deleteData: true` is supplied.
Operations have a ten-minute CLI limit; listener callbacks run on a separate
bounded queue outside both operation and listener locks. Only one install or uninstall is accepted at a time. The store
tags each request with an operation ID and ignores completion events for any
other request. Access is checked with the bridge's
`org.matonos.permission.SYSTEM_BRIDGE` permission.

## Build and verification

- The service is compiled by the coordinating AOSP build; do not run `m` or
  `lunch` from this area.
- `linux/dbus-broker/build-android.sh` builds the standalone broker with the
  Android NDK into `out/pc-logs/dbus-broker/android`.
- Run `tools/preflight.sh` before requesting an image build.
- Runtime verification requires a freshly booted image: install, launch, and
  uninstall a harmless Flathub app; confirm option-looking run args are
  rejected, no-data uninstall preserves app data, progress/completion reach the
  store, and the system bridge is the only caller accepted by linuxd.

## Real hardware check

1. Boot the new image on the Ryzen/RX 6600/Intel 7265 build PC, Surface Pro 3
   (Marvell 88W8897), and HP ProDesk 600 G1. First boot each with networking
   disconnected and confirm Android reaches the launcher; this exercises the
   no-Flathub/no-operation fallback.
2. Connect networking, open Software Center, install a small app such as
   Calculator, and wait for its matching completion event. Confirm the busy
   state clears and the Installed list refreshes.
3. Launch the installed app, then uninstall it with the default choice and
   confirm app data is retained. Repeat with explicit `deleteData: true` and
   verify that data is removed.
4. From an authorized bridge test call, pass a run argument beginning with
   `--filesystem=` and confirm linuxd rejects it. Pass a normal positional
   argument and confirm Flatpak receives it after the `--` delimiter.
5. While an install is active, submit a second install and confirm linuxd
   rejects it promptly. Kill the linuxd process and confirm init restarts it
   without affecting boot or the rest of the running system.

## Launcher stubs

The system bridge links `MatonLinuxStubGenerator` and owns stub signing in its
Android Keystore. It reconciles installed refs at startup, after completion
notifications, and every 30 seconds. The validated `desktop_entry` command
returns the installed deployment's exported desktop file. Generated packages
are installed through PackageInstaller and removed after Flatpak uninstall.
The signing alias survives Software Center updates and removals; clearing the
bridge's own data still removes its key.

Linuxd and the bridge use matching one-way progress listener definitions.
The store shows numeric progress when Flatpak reports percentages and an
indeterminate bar during other stages.

The validated `icon` command returns base64 PNG data. Lookup inside the
deployment root, in order:

1. exported `export/share/icons/hicolor/<size>/apps/<ref>.png`;
2. appstream-compose's `files/share/app-info/icons/flatpak/<size>/<ref>.png`
   (covers apps whose hicolor export is SVG-only, e.g. Brave);
3. AppStream media thumbnails under
   `files/share/app-info/media/<ref as path>/<release>/icons/<size>/<ref>.png`;
4. the Flathub repo's extracted appstream branch under
   `/data/matonos/linux/flatpak/appstream/flathub/<ref as path>/active/icons/`.

Every candidate must resolve inside its root, stay under 256 KiB, and carry
the PNG signature; otherwise the stub keeps its fallback icon.

## Open issues

- Display selection: launches export both `WAYLAND_DISPLAY` and the
  per-session `DISPLAY` (Xwayland socket path) and both sockets; toolkits
  self-select (GTK/SDL2 -> Wayland, Qt and Chromium/Electron -> X11). Per-app
  overrides are planned through the bridge's generic channel
  (`set_display_mode`), not a config file. Xwayland launch verification is
  pending a fresh image.
- Runtime confirmation of automatic stub installation is pending the local
  repair image build.
