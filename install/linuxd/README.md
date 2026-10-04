# Flatpak linuxd

`matonos-linuxd` exposes the Flatpak manager to the system bridge through the
existing `ILinuxd` Binder interface. It accepts structured JSON only, validates
refs and app IDs before invoking the fixed
`/apex/com.matonos.flatpak/bin/flatpak-env-wrapper` launcher (the Flatpak stack ships in the
updatable `com.matonos.flatpak` APEX), and runs package operations on a worker
while reporting progress. It starts
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

## H5 phase 1: session pads (relay deferred)

`SessionPads.h` is a plain C API. Create up to four Xbox 360 uinput pads
from a descriptor-keyed `MatonPadInventory` array and the verified stub UID;
Linux evdev axis codes/ranges are supplied by the future Android relay.
Missing axes use Xbox defaults. The session owns all FDs, its FF worker and
a comma-separated node-list string. `maton_pads_send` accepts bounded,
validated button/axis/SYN_REPORT batches; `maton_pads_release` resets buttons,
sticks/hats and triggers and cancels rumble. Destroy joins the worker,
releases inputs, destroys kernel pads, closes FDs and refreshes the udev DB.
FF advertises rumble only when both inventory and callback support it;
unsupported effects or delayed starts return ENOSYS. The callback reports
strong/weak magnitudes, and zero/zero on stop/erase/timeout/release/teardown.
It runs serialized with FF handling and must not reenter/destroy the session.

Normal launches currently create an empty inventory and export an empty
`MATON_SESSION_PAD_NODES`; no physical input nodes are exposed. The launcher
preserves this field across clearenv, and bwrap reads Flatpak's bundled
arguments as well as the direct environment. It binds only enumerated event
nodes, keeps `/run/udev` and `SDL_JOYSTICK_DISABLE_UDEV=1`, and never binds
hidraw or uinput. Controller permission/group retirement is a separate phase.
Future relay work must check identity and permission, create pads before
spawn, then attach the session to the reaper and forward normalized batches.
Hotplug after launch is deferred to the next launch.

Host check (no devices):

```
cc -Wall -Wextra -Werror -pthread install/linuxd/tests/session-pads-test.c -o /tmp/session-pads-test
/tmp/session-pads-test
```

Optional local kernel hook (not run in phase 1): compile
`tests/session-pads-local.c` with `SessionPads.c`, `UdevDatabase.c` and
`-pthread`. On a disposable authorized target it creates one pad owned by
UID 10000, prints its node, logs FF callbacks, and destroys it on Enter.
This is a local C hook, not a socket protocol or Android vibrator relay.

## linux-data task, 2026-10-04 (narrowed scope)

The sandbox domain is `matonos_linux_app`. Persistent writable state uses the
existing `/data/matonos/linux/apps/<stub uid>/home` path, labelled
`matonos_linux_data_file` with the verified stub's MLS categories. Linuxd
creates the UID tree and home using component-wise openat/O_NOFOLLOW walking,
0700 mode and stub UID/GID ownership. The existing system-owned `<uid>.owner`
record is retained to reject reuse by a different Flatpak. The wrapper checks
ownership and uses home for HOME, user data and cache; bwrap binds that home.
The mount helper no longer attaches vol.img. Shared repo/cache/store paths stay
in place. Dev images need no migration: reset userdata for an older layout;
existing wrongly labelled directories fail closed rather than being relabelled.

Display/socket ownership, Wayland relays, Xwayland handling and the launch AIDL
arguments are restored to the baseline. No per-UID run directory is created.
The compositor sources and both ILinuxd copies have no diff. The only retained
FlatpakManager/MatonosLinuxd launch-path edit prepares the verified data tree;
no compositor listener FD handoff remains.

Project IDs use the checked-out installd formula `uid - 10000 + 50000`:
`frameworks/native/cmds/installd/utils.cpp:438-440`,
`InstalldNativeService.cpp:866-876`, and
`system/core/libcutils/include/private/android_projectid_config.h:54`.
`frameworks/base/core/java/android/os/storage/StorageManager.java:2420` documents
matching native ID ranges; `:2502-2504` adds the per-user range. Vold uses
FSGETXATTR/FSSETXATTR in `system/vold/Utils.cpp:240-265` and the analogous
UID-offset external-data scheme at `:395-410`. Linuxd sets the app ID and
PROJINHERIT before chown on newly created directories. Unsupported/failed
quota ioctls log a warning and never block boot or app launch. Existing files
are not recursively retagged; actual StorageStats accounting requires a fresh
image test on a project-quota filesystem.

The bridge's startup/periodic reconcile inventories numeric UID directories
through linuxd and requests deletion when PackageManager reports no packages
for that UID. Package/user removal therefore converges on the same cleanup.
Reused UIDs are conservatively retained; the owner record rejects reuse by a
different Flatpak. Deletion walks FDs, never follows symlinks, and refuses a
child directory on another device. Cleanup failure is logged and retried.

Policy permits only the Linux app domain to execute this data type, denies
all appdomain access, and keeps the existing sandbox Binder neverallows plus
its mandatory seccomp Binder ioctl rejection. Stock domain.te grants a minimal
Binder baseline to all domains; our private policy cannot subtract it. Do not
claim a literal SELinux-only denial of every Binder operation.

Validation: full policy rig PASS with fresh platform CIL, both neverallow
checks and merged secilc without -N; build-native.sh PASS; freshly generated
bridge AIDL + bridge/stubgen javac PASS; linuxd C/C++ and AIDL object compile
PASS; NDK bwrap and mount-helper compile/link PASS. No compositor Gradle build
is required because its files were restored. Preflight fails only on 19 missing
worktree APK imports and the approved 0001 patch absent from its filename
allowlist. No shared image build or fresh QEMU test is claimed.

Real-hardware tests (after coordinator rebuild):
1. Boot a fresh enforcing QEMU image on an owned 5556+ port, disable sleep idle,
   run check-selinux-labels.sh on the image and verify normal boot completion.
2. Launch signed Wayland and X11 stubs; confirm the original compositor sockets
   and display selection, plus UID-owned 0700 home with the stub MLS categories.
3. Download a small executable into home: execution/map succeeds only in the
   Linux sandbox; the stub and unrelated apps cannot access it. Verify Binder
   rejection and no execmod. Test two stubs and a secondary user for isolation.
4. Uninstall a stub and remove a user; wait for reconcile and verify UID tree
   and owner-record deletion. Reinstall and confirm no stale data. Exercise
   symlink/foreign-owner/wrong-label rejection without affecting normal boot.
5. Compare project IDs and StorageStats on a quota-capable filesystem, then
   repeat steps 2-4 on Ryzen 5800X/RX6600/Intel7265, Surface Pro 3 and HP ProDesk
   600 G1. These runtime tests remain outstanding; no kernel change is needed.
