# Installer service

The app-authored request contract is in [AB-LAYOUT.md](AB-LAYOUT.md), and the
ordered v1 install sequence is in [PLAN.md](PLAN.md). MatonOS Settings remains
the caller; its signed package is authorized for the `install` bridge target.
The service does not choose a layout or sequence.

## Service design

`service/InstallerDaemon.cpp` registers `get_status`, `list_drives`,
`execute_operation`, and `cancel_operation` on
`vendor.matonos.channel.IChannel/install`. Each write takes a fresh drive
snapshot and runs the existing operation validation before reaching
`InstallerExecutor.cpp`. The app carries a selected-drive identity token in
every primitive request. It combines the canonical sysfs device path, Linux
`diskseq`, and normalized WWID/serial when available; `major:minor` remains
only a current node locator. Missing diskseq/path, an identity mismatch, or
a duplicate identity fails closed and asks the user to refresh and confirm
again. At the write boundary, direct writers check `BLKGETDISKSEQ` on the
opened block descriptor and keep it open for the operation. GPT/formatter
helpers pass that verified descriptor to the tool through
`/proc/self/fd/<n>`, preventing later reuse of the device number from
redirecting a write. The daemon is started asynchronously only when
`ro.boot.matonos.live=1`; the gate reads `/proc/cmdline`, and an absent daemon
or target disk does not block boot. It fails closed if mount, swap, or kernel
command-line or disk identity state cannot be read before a write.
It uses the fixed `matonos_driver` SELinux domain from the device policy.

The executor implements GPT writes through the shipped `sgdisk`, ext4/FAT
formatting through shipped system tools, bounded block copies and clones with
readback verification, an ODM-side A/B LP metadata writer, and direct FAT32
file reads/writes for the fixed boot-file catalog. The LP metadata writer was
validated against the host `lpdump`; the FAT writer passed a host round-trip.
The integrated daemon has booted from a coordinated image and passed the
physical-device replacement test in QEMU. User-file migration is intentionally
skipped: installs start with empty `/data` and do not include a staging or
import path. The A/B operation sequence and target-only boot still need fresh
image verification.

Current implementation adds A/B dynamic partitions, a boot-control HAL,
signed per-slot UKIs, and systemd-boot counting entries. The live super has
slot A only; install copies A and clones it to B. The service runs on live
boots only, and Settings hides the Install alias on installed boots. Installed systems initialize an empty `/data` with fresh FBE state. There is
no user-file operation, staging volume, FUSE copy path, or first-boot importer.
No AOSP patch is used. The installer channel remains in the fixed
`matonos_driver` domain. Common NVMe, MMC, SATA/SCSI, and virtio block nodes
receive a specific SELinux type rather than the generic block-device type
forbidden to vendor domains; the service checks the captured physical identity
before writing. Fixed GPT and formatter commands run through the
existing `matonos_driver_helper` execution path because the Bluetooth HAL
server domain cannot execute arbitrary system tools. Boot file transfer uses
direct FAT operations because vendor domains cannot mount filesystems under
platform policy.

## Build and test

- `bash install/build-core.sh` compiles the NDK-safe service core.
- `native/matonos-installer-service/CMakeLists.txt` is the out-of-Soong NDK
  daemon target and reuses the core from `install/service/`.
- `install/tools/test-install.sh --list` lists drives through the debug-only
  bridge test call. `--install <major:minor>` submits the same
  `OperationRequestV1` shapes as the Settings planner, but refuses to erase a
  disk unless its QEMU serial is `MATONTGT`. The test hook requires a root
  shell, a debuggable live image, and
  `persist.vendor.maton.installer_test=1`. The script submits the planned
  `OperationRequestV1` sequence: GPT, filesystem formats, A/B LP table, fixed
  ESP payload, source A copies, A-to-B clones, UKI payloads and slot entries.
  The target serial and captured sysfs identity are checked before each
destructive request. The test harness closes ADB
  stdin so the shell client cannot consume the remaining operation file. On a
  fresh boot it connects to the ADB TCP endpoint, waits for the device, and
  retries channel readiness for up to 30 seconds before sending writes.
- Hotplug identity check: capture a disposable target's drive ID and identity,
  detach it, and attach a different qcow2 target until it receives the same
  `major:minor`. Submit the old `write_gpt` request; it must fail before
  starting `sgdisk`. Repeat after one successful primitive by swapping the
  target before the next operation. The request retains the selected
  identity, and the opened-fd `diskseq` check also protects the race between
  enumeration and opening the target.
- The disposable target is `/mnt/data/aosp/out/pc-logs/install-target.qcow2`
  and replacement is `/mnt/data/aosp/out/pc-logs/install-replacement.qcow2`
  (64 GiB sparse qcow2 files). Both were attached through a QEMU PCIe root
  port to allow QMP hot-unplug/replug.

## Real hardware checks after completion

1. Ryzen 5800X / RX 6600 / Intel 7265: check every enumerated disk and serial,
   verify the live disk, mounted disks, read-only media, and drives below the
   app's minimum are disabled; install only to an explicitly selected blank
   SSD and boot it alone.
2. Surface Pro 3: verify the internal Marvell storage is not mistaken for the
   live source; install to a blank external SSD and verify both slots.
3. HP ProDesk 600 G1: check boot with no optional network devices and confirm
   the installer daemon cannot delay boot when it fails or no target exists.

## Latest verification

- `install/build-core.sh`: passed after adding identity and opened-device
  `diskseq` verification (NDK r30, x86_64, API 35).
- Installer daemon target in `out/pc-native/matonos-installer-service`:
  passed after compiling the service, direct writers, and verified-fd helper.
- `tools/build-apps.sh`: passed; the Settings APK containing the identity-bound
  planner was rebuilt and staged. `tools/preflight.sh` passed.
- `MATON_BUILD_COORDINATOR=1 tools/build-native.sh`: the aggregate attempt
  stopped in the unrelated `addons/native/matonos-addons-service` project
  before reaching the installer target. The installer daemon target was then
  built directly with the NDK CMake build and staged successfully.
- Host-side fresh-snapshot regression passed: a changed identity at the same
  `major:minor` is rejected, duplicated serial identity is rejected, and the
  unchanged selected disk is accepted.
- `install/build-core.sh` and `MATON_BUILD_COORDINATOR=1 tools/preflight.sh`:
  passed after the final service changes.
- Direct FAT32 source-to-target round-trip: passed with `mkfs.fat`, `mtools`,
  and the service library; `mdir` independently listed the written EFI file.
- A generated A/B LP metadata image parsed successfully with AOSP host
  `lpdump`, including all ten requested logical partitions and extents.
- Fresh 2026-09-28 image boot: `sys.boot_completed=1`; `ro.boot.matonos.live=1`;
  SystemBridge and Settings packages were installed. The debug bridge call
  registered the installer channel and listed NVMe serial `MATONTGT` as safe,
  while identifying the live USB disk as unsafe.
- Fresh QEMU install run against the 64 GiB `MATONTGT` NVMe: GPT write and
  independent `sgdisk --print` verification passed; ESP, boot, both add-on
  filesystems, userdata, and A/B LP metadata operations passed. BSD
  `newfs_msdos` requires `-L` for the label and `-c 8` to make the 512 MiB ESP
  large enough for FAT32. The updated image with these flags was built and
  booted successfully.
- The previous 2026-09-28 04:53 image includes raw primary-GPT verification
  (header and partition-array CRC, partition GUID and extent checks) before
  accepting ESP/XBOOTLDR writes. The full bridge request run passed GPT and
  `sgdisk` readback, five formats, LP metadata, both ESP writes, live A copies
  for all five logical partitions, boot A files, all five A-to-B clones, boot
  B files, and both loader entries. That earlier run stopped at operation 23 for user-file migration. User later
  chose to omit migration entirely; this is historical evidence only. See
  `out/pc-logs/install/install-run-gpt-verified.log`.
- After the live VM shut down, a separate 4 GiB QEMU VM booted the target
  NVMe alone (no live disk). OVMF and systemd-boot discovered it and showed
  both A/B entries. Android did not reach adb/home (`127.0.0.1:5557` remained
  offline). This is consistent with the then-current non-A/B product. The current A/B conversion
  must be checked on a newly built image; the earlier image is not proof of
  installed boot.
- Full coordinated image build passed at 2026-09-28 02:37. The image was
  freshly booted with the debug entry; `sys.boot_completed=1` and
  `init.svc.matonos-installer-service=running`. `tools/check-selinux-labels.sh`
  reported no FAILs (all nine init-started vendor programs had exec labels).
- QEMU replacement before the first write: the original target was `254:0`,
  with sysfs identity ending `seq=3`. After QMP unplug/replug, the replacement
  was also `254:0` with identity ending `seq=166`. The original `write_gpt`
  request failed with “The selected physical drive changed.” Both qcow2 hashes
  stayed at the zero-disk baseline
  `19eb403b49ebb8007177a5f8cc49713c69a517d81c30fb31673336e5dd298cc3`.
- Replacement between primitives: `write_gpt` succeeded on the selected
  replacement. After unplugging it and reattaching the original image at
  `254:0` (new `diskseq=167`), the next request with the replacement's old
  identity was rejected with the same message. The original qcow2 hash stayed
  at baseline; the GPT-written replacement hash stayed
  `ce41a98d6be1237295eeefbdeb52b14586a368ec27e7fa1670831d1b3bc97568`.
  After shutdown, host `sgdisk` found no partitions on the original and only
  the one `sentinel` GPT partition on the replacement. The disposable VM was
  shut down after collecting evidence.


## A/B installation update (2026-09-28)

The current implementation converts the installed image to A/B logical system
partitions. The live super contains only slot A. The installer creates both LP
groups, copies A, clones A to B, writes signed slot-specific UKIs, and creates
systemd-boot counting entries. A MatonOS boot-control HAL persists slot state
in misc and controls the loader default without EFI NVRAM. Add-on partitions
addons_a and addons_b are each 512 MiB.

The live installer service and Settings Install icon are gated by
ro.boot.matonos.live. The current user decision skips file migration. Installs have no stage partition
or importer, and installed `/data` begins empty.

### 2026-09-28 target install retry

The live VM booted with `sys.boot_completed=1`, live mode set, and the
installer service running. Its test bridge listed the blank 64 GiB NVMe as
safe with serial `MATONTGT`. The 20:13 live package passed GPT write/readback,
formatting, LP metadata creation, ESP payload writes, all five A copies, and
all five A-to-B clones. In a target-only QEMU boot, OVMF found the ESP,
systemd-boot listed slots A and B, and slot A launched its UKI. Android
  first-stage init then panicked because `ReadDefaultFstab` could not find the
  default fstab; it did not reach adb or the home screen. The planner and test
  script now pass hardware, installed fstab suffix, slot suffix and boot-device
  path explicitly in each entry. The installed fstab changes require a fresh image. The AIDL source-root issue
  was fixed with a filegroup rooted at `aidl`. After the user removed migration,
  preflight passed and native installer compilation succeeded. The latest
  coordinated full build compiled `MatonosLinuxd.cpp` but failed at link time:
  Android Binder/String symbols and the `flatpak_manager_*` functions were
  unresolved. The exact diagnostics are in `build-status.txt`; this build
  blocker is outside the installer area. No image containing this revision has
  been produced yet. Target-only boot,
  slot checks and installer absence remain unverified. No
  target home screenshot exists yet.

## Flatpak bridge service (v2, 2026-09-28)

`matonos-linuxd` is a system_ext native process with a private local Binder
interface, `org.matonos.systembridge.ILinuxd/default`. The system bridge's
generic `flatpak` target forwards its `call()` and `subscribe()` operations to
that service after enforcing the normal target ACL; the image-signed
MatonOS Settings app is listed as the built-in caller. A root-only test call
is exposed through the bridge shell provider only on a debuggable live image
after `persist.vendor.maton.flatpak_test=1`.

The service invokes only `/system_ext/bin/flatpak` using `posix_spawn` and a
fixed argv per command; no shell or `flatpak-spawn --host` is involved. It
accepts complete `app|runtime/ID/ARCH/BRANCH` refs, checks Flatpak app IDs,
and rejects unknown JSON fields. The system installation and app data live
under `/data/matonos/linux`; the tree receives the dedicated
`matonos_flatpak_data_file` SELinux label. Because the service and bridge are
both on the system side, the service uses a small system AIDL and is not a
vendor channel or VINTF instance. The bridge and native service keep matching
copies of these two AIDL declarations under their own Soong source roots.
Install/uninstall output is sent as
`progress` events and returns an accepted job immediately, so a long pull does
not hold a bridge Binder call open. Completion arrives as a `progress` event
with `phase=complete` and the CLI result. App data is placed under
`/data/matonos/linux/flatpak-data/.var/app`. Uninstall requests
`--delete-data` then removes unused runtimes. The service starts only after `sys.boot_completed=1`, so a missing
CLI or Flatpak failure cannot hold up boot. D-Bus is not enabled by this
service. The daemon uses its dedicated `matonos_linuxd` system coredomain and
existing fixed system_ext policy files; SELinux rejected placing its system
executable or `/data` tree in the vendor `matonos_driver` domain.

One integration condition remains open: upstream Flatpak enables its system
helper by default, and its non-root system-install path contacts that helper
over the system bus. `matonos-linuxd` currently runs as `system`, so verify the
spike's actual build flags and an install attempt before claiming the no-D-Bus
path works. If the helper is enabled, either the CLI needs a supported direct
write configuration or the system bus/helper must be included; keep D-Bus off
unless the runtime test proves the helper is required.

### Build and QEMU check

The coordinator builds `matonos-linuxd` in the full image. After a fresh
headless QEMU boot, run the bridge `flatpak_test_call` with commands
`list_installed`, `list_remotes`, `add_flathub`, `install`, and `uninstall`;
watch `progress` for install and uninstall. A CLI test ref is
`app/fi.mooc.tmc.tmc-cli-rust/x86_64/stable`. Confirm its ref disappears from
`list_installed` and `/data/matonos/linux/flatpak/app/fi.mooc.tmc.tmc-cli-rust`
(and its matching app data) is gone after uninstall. Also exercise `run` and
`kill`; the test runner must verify the returned PID and process exit.

The 2026-09-29 05:59 full image was copied to
`out/pc-logs/linuxd/matonos-test-copy-0559.img` and booted in QEMU on adb port
5562. `persist.vendor.maton.sleep_idle_s=0` was set after boot so the test
session stayed awake.
`ro.boot.matonos.live` was `1`, `init.svc.matonos-linuxd` was `running`, and
`ps -AZ` showed the daemon in `u:r:matonos_linuxd:s0`. The binary label was
`matonos_linuxd_exec`; `/data/matonos/linux/flatpak` had
`matonos_flatpak_data_file`. The root/live `flatpak_test_call` hook reached the
daemon: `list_installed` and `list_remotes` returned exit code 127, and
`add_flathub` returned exit code 127. The missing-CLI guard stopped at
`/system_ext/bin/flatpak --version` with `/system/bin/sh: ... inaccessible or
not found` (exit 127). Install and uninstall hook calls were accepted as
asynchronous operations, but no package was installed and no deletion was
verified; the Flatpak executable was absent. `run` initially exposed a dead-PID
response when the CLI was missing. `run_async` now checks executable access
first; the 05:59 image returned `{"error":"No such file or directory",
"exitCode":127,"ok":false}` through the bridge hook. Repeat the
install/uninstall and data-removal test once the spike image includes Flatpak.
On real hardware, repeat these calls on the Ryzen 5800X/RX 6600/Intel 7265,
Surface Pro 3, and HP ProDesk 600 G1; no optional PC hardware is required for
boot or service startup.

Earlier 2026-09-28 retries did not reach this service: bubblewrap first failed
on its C23 `bool` typedef and later on bionic's missing
`get_current_dir_name`; another build stopped during Soong graph generation
because the compositor tree duplicated AOSP's `libffi` modules. Those spike
and compositor blockers were addressed far enough for Soong analysis to
complete. The daemon compiled and linked in the 2026-09-29 04:14 image after
fixes for the generated-header include root, AIDL C++ namespace, String16
conversion, linker dependencies, C ABI declaration, and platform-side file
labels. A later build exposed the bridge hook's static call to instance-only
`linuxdFor()`; the hook now routes through `activeService` and the corrected
bridge compiled in the successful 04:14 image. The 05:59 full image compiled
the follow-up `run_async` executable check, and a fresh QEMU copy verified the
missing-file error response. The Flatpak CLI and its GLib/OSTree dependencies
remain outstanding in the spike build sequence.

## 2026-09-29 fresh A/B install attempt

The full image build succeeded at 05:59 and the Treble label check reported no
failures. Live QEMU reached `sys.boot_completed=1`,
`ro.boot.matonos.live=1`, slot `_a`, and
`init.svc.matonos-installer-service=running`. The installer channel reported
executor/readback checks ready and listed the blank 64 GiB target NVMe
(`MATONTGT`, 259:0) as safe.

Corrected test script operations 1-9 succeeded on the target: GPT and
independent `sgdisk` readback, ESP and boot FAT formats, metadata/add-on/
userdata ext4 formats, A/B LP metadata, and EFI payload writes. The coordinator
started a previously queued GLib build and terminated the live VM during
operation 10 (loader configuration write). No logical partitions were copied
and the target was not boot-tested in this attempt. Two later VM attempts were
interrupted by coordinator-started GLib dependency builds. The first stopped
before ADB came online; the next reached ADB with
`ro.boot.matonos.live=1` and
`init.svc.matonos-installer-service=running`, then was stopped before the
installer script ran. The 64 GiB `install-target-rerun.qcow2` remains blank.
Earlier dependency-build status notes below are historical; the coordinated
build and install test results are recorded in the dated sections that follow.

## 2026-09-29 A/B conversion and target boot follow-up

The coordinated full build completed at 09:05:21 (image timestamp 09:05:13).
The byte-identical copy at `out/pc-logs/install/matonos-live-20260929.img`
booted with the installer service running and the blank `MATONTGT` 64 GiB NVMe
reported safe by the bridge. `install/tools/test-install.sh --install 259:0`
completed all 21 bridge `OperationRequestV1` calls, including the GPT,
filesystem formats, A/B LP metadata, five live A copies, five A-to-B clones,
and per-slot EFI loader entries. Independent `sgdisk` readback showed the
expected `addons_a` and `addons_b` 512 MiB partitions. Host `lpdump` parsed the
target metadata and found both groups and all ten slot-suffixed logical
partitions.

The first target-only boot selected `_a` and mounted every logical partition
plus `/data` from the target NVMe, but could not complete framework startup.
On the virtio-vga retry, the stock DRM composer was denied creation of its
kernel uevent socket and crashed before SurfaceFlinger could start. The device
policy now grants that socket permission. Boot-control also reported a failed
`misc` discovery; its read path now logs the discovery error and falls back to
current-slot defaults so a missing state device cannot block early boot. Slot
state writes still require `misc`. These follow-up changes passed the NDK
native build and `tools/preflight.sh`; a new coordinated full image and fresh
QEMU run are required before claiming target boot completion.

The live image did reach `sys.boot_completed=1` with the service running. The
headless `-g none` screenshot was black, so there is no visual live home-screen
evidence from this run. Retry with the non-virgl `virtio-vga` display device to
capture the installed home screen after the policy rebuild. User-file migration
is removed by decision: no staging partition, FUSE route, copy operation, or
first-boot importer exists, and the installed system starts with empty `/data`.

The fresh target-only retry after the policy rebuild selected `_a` and started
`matonos-bootctrl`; the graphics composer no longer hit the earlier uevent AVC.
Boot still stopped before home because the stock software Gatekeeper HAL tried
to register its `ISharedSecret/gatekeeper` AIDL subservice. A follow-up build
confirmed that AOSP policy intentionally reserves `hal_sharedsecret_service`
registration to KeyMint; a device allow rule violates a platform `neverallow`.
Per the coordinator's instruction, retain the Gatekeeper APEX and label its
companion interface as `hal_gatekeeper_service`, which the existing Gatekeeper
HAL domain can register. Rebuild and re-run the target-only boot before claiming
completion; the prior target image's boot result is not final evidence.

## 2026-09-29 A/B image and bridge install retry

The full image completed at 12:58:16, and
`tools/check-selinux-labels.sh` reported all nine init-started vendor programs
correctly labeled. The image copy
`out/pc-logs/install/matonos-live-20260929-ab-final.img` booted in a fresh
4 GiB QEMU VM. It reached `sys.boot_completed=1`, reported
`ro.boot.matonos.live=1`, and the bridge reported the installer executor,
dry-run, and readback checks ready. The bridge listed the blank 64 GiB
`MATONTGT` NVMe as safe. The 21 `OperationRequestV1` test calls all returned
success, writing the GPT, formatting all ordinary and add-on partitions,
creating both LP groups, copying the five live A partitions, cloning all five
to B, and writing the per-slot EFI loader entries. GPT readback showed the
two 512 MiB `addons_a` and `addons_b` partitions.

The target-only QEMU boot selected the A entry and passed
`androidboot.slot_suffix=_a`; serial output showed target logical partitions
and userdata mounted from the target NVMe. It did not reach Android home on
this build. First boot spent several minutes decompressing APEX packages into
fresh userdata. After that, SurfaceFlinger and drm_hwcomposer repeatedly
aborted with `gralloc-mapper is missing`. The AVC log identifies the cause:
`hal_graphics_composer_default` was denied `find` on the AIDL graphics
allocator service. The keystore log also showed `find` denied for the
Gatekeeper shared-secret companion after it was correctly registered with the
authorized `hal_gatekeeper_service` type.

The existing fixed `sepolicy/matonos/matonos_driver.te` now declares the DRM
composer as an allocator client and keystore as a Gatekeeper client. These
permissions use AOSP HAL client macros; no AOSP patch or new policy file was
added. The change passed `tools/preflight.sh`. It still requires a fresh image
and fresh QEMU live/install/target-only run. The live image reached boot
complete, but this no-display run did not provide a visual home-screen
confirmation. The attempted target image and diagnostics are
`out/pc-logs/install/install-target-20260929-ab-final.qcow2` and
`out/pc-logs/install/serial-ab-target.log`.

## 2026-09-29 fresh A/B image and retry

The coordinated full image completed at 14:05:50 (image timestamp 14:05:37).
The fresh live copy at `out/pc-logs/install/matonos-live-20260929-ab-home.img`
booted with `sys.boot_completed=1`, `ro.boot.matonos.live=1`, and the installer
channel registered. On a blank 64 GiB NVMe, the bridge test hook completed all
21 operation requests; its captured transcript is
`out/pc-logs/install/bridge-install-ab-home.log`. The target GPT readback showed
`addons_a` and `addons_b`, each 512 MiB, plus `super`, `misc`, `metadata`, and
`userdata`. Five A partitions were copied from the live image and cloned into
the B slot.

The target-only firmware menu displayed both entries and selected A. Serial
confirmed `ro.boot.slot_suffix=_a` and an unset `ro.boot.matonos.live`. APEX
decompression on fresh userdata took several minutes. The current target boot
then exposed a remaining graphics service-label problem: drm_hwcomposer
repeatedly fails `find` for `mapper/minigbm`, which was labeled
`default_android_service`, and SurfaceFlinger/Launcher3 abort. I added the
service mapping to `hal_graphics_mapper_service` in the existing fixed
`sepolicy/matonos/service_contexts`; it needs a new full image and another
target-only boot before home-screen completion can be claimed.

The live UI boot completed in headless `-g none` mode, but its screenshot was
black. Non-virgl standard VGA showed the live lock screen; dismissing it led to
repeated Launcher3 `llvmpipe` crashes, so this run does not establish a usable
live home screen. Logs and screenshot attempts are under
`out/pc-logs/install/serial-ab-home-live*.log` and
`out/pc-logs/install/live-ab-home*.png`.

The importer decision is fully reflected in implementation: searches of the
installer service, bridge, Settings planner, fixed SELinux policy, and install
tools find no user-file copy request, staging partition, FUSE route, or import
service. The installed `/data` starts empty. Remaining mentions in the install
documents are decision/history notes only.

## 2026-09-29 fresh target boot with corrected loader options

The coordinated 15:26:20 full image was copied to
`out/pc-logs/install/matonos-live-20260929-ab-mapper.img` (SHA-256
`070d1e66ddf77bc70267a7eb4e7d95be5510809f201722cb5d9498e3f9a61b2a`). The
live image reached `sys.boot_completed=1`, reported live mode, and exposed the
installer bridge. A newly blank 64 GiB NVMe (`MATONTGT`) was installed through
all 21 bridge operations. The transcript is
`out/pc-logs/install/bridge-install-ab-kargs.log`; GPT readback shows ESP, boot,
misc, metadata, 10.5 GiB super, `addons_a`, `addons_b` (512 MiB each), and
userdata. Offline `lpdump --all` confirms `system`, `system_ext`, `product`,
`vendor`, and `odm` in both A/B groups. Evidence is in
`out/pc-logs/install/target-kargs-lpdump.txt`.

The first target-only attempt exposed incomplete kernel options in the
generated systemd-boot entries: the installed kernel used SELinux enforcing,
so Mesa/minigbm failed to load after policy denials and libui reported a
missing gralloc mapper. This was not a VINTF blocker; the earlier VINTF
diagnosis is withdrawn. The coordinator updated both entries in
`install/tools/test-install.sh` and the Settings planner with the live boot
options, including `androidboot.matonos.live=0`,
`androidboot.selinux=permissive`, `androidboot.verifiedbootstate=orange`,
firmware path, and VGA/serial console settings. The 15:26 image needs no rebuild
for this host-side plan update. A rerun with the corrected entry is pending.

The earlier diagnostic boot logs, including the initial checkpoint reboot,
describe an install made with the incomplete command line and do not establish
the result of the corrected plan. The installed home screen, boot-complete
state, successful slot blessing, and screenshot remain unverified. The
user-data importer remains fully removed and installed `/data` is empty.

## Fresh target retry with FAT directory inspection (2026-09-29)

The coordinated 18:14 image booted the live session to
`sys.boot_completed=1` with `ro.boot.matonos.live=1`. A blank 64 GiB NVMe was
installed through the bridge test hook; all 21 requests succeeded. The
transcript is `out/pc-logs/install/bridge-install-ab-fixedboot.log`.

On target-only boot, OVMF selected slot A but reported
`Failed to rename '\\loader\\entries\\A+3-0.conf' to 'A+2-1.conf': Invalid
parameter`; Android did not expose ADB within four minutes. Offline
`fsck.vfat -vn` inspection found duplicate 8.3 aliases (`MATONO~1.EFI`) for
the two `matonos-{a,b}.efi` payloads and invalid `..` entries for directories
whose parent is the FAT32 root. `install/service/FatFilesystem.cpp` now
chooses a unique short alias and uses cluster zero for the root parent entry.
The source compiles with `tools/build-native.sh`; a coordinated build and fresh
install/target-only retest are pending. ESP diagnostics are under
`out/pc-logs/install/target-ab-fixedboot-esp.img` and
`out/pc-logs/install/target-ab-fixedboot.raw`.

The 19:49 coordinated full build recompiled and staged the installer service,
then failed on another app's `<uses-library>` check (`MatonFossifyClock`). Its
retry failed before native staging at the MatonFlathub `npm ci` step. The
bundle-only request also entered the full build path and hit that same npm
failure. No new bootable image was produced; the 18:14 image remains the last
successful image, so the repaired ESP writer has not yet been exercised in
QEMU.

The earlier plain-entry diagnostic boot reached Android userspace but used the
incomplete installed command line described above. It does not establish the
result of the corrected A/B plan. No VINTF blocker was confirmed. The installed
home screen, boot-complete state, successful slot blessing, and screenshot
remain unverified pending the rerun.

At 21:23 on 2026-09-29, the coordinator started a quick build while the
corrected-command-line live retry was still booting. The coordinator stopped
the VM under the shared build rule before ADB came online or the installer
bridge ran. This attempt produced no install result; retry with a fresh blank
target after the build queue clears.

## AOSP OTA compatibility check: legacy dm-snapshot Virtual A/B (2026-09-29)

The requested Android 11 Virtual A/B path (`virtual_ab_ota/launch.mk`, with
neither compression nor snapuserd) is not supported by the current AOSP
`libsnapshot` update path. In `system/fs/fs_mgr/libsnapshot/snapshot.cpp`,
`CreateUpdateSnapshots` disables userspace snapshots when the target payload
does not declare VABC metadata, then returns an error for the legacy
dm-snapshot path (`Using legacy Virtual A/B (dm-snapshot)`). Thus setting the
`launch.mk` feature flag alone does not provide a working OTA path.

This AOSP does support uncompressed COW data with the
`virtual_ab_ota/vabc_features.mk` configuration (`PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := none`),
but that configuration enables userspace snapshots and packages snapuserd.
It does not meet the no-snapuserd requirement. The installer layout was not
converted based on this result; implementing the requested update path would
require an AOSP change, which the zero-AOSP-patches rule currently excludes.

## Corrected-command-line live install retry (2026-09-30)

The saved 15:26 image booted live successfully on port 5565 with
`sys.boot_completed=1`, `ro.boot.matonos.live=1`, and
`init.svc.matonos-installer-service=running`. The bridge listed the blank
64 GiB NVMe as safe with serial `MATONTGT`; it excluded the live USB image.
The test script's `sgdisk --print` readback verified the 8-partition GPT,
including `addons_a` and `addons_b` at 512 MiB each. Formatting, A/B LP
metadata, both EFI payload writes, and copies to all five A logical partitions
returned success. The `system_a` to `system_b` clone returned success.

The first run's operation 12 was followed by a transient shell `content call`
ActivityManager/provider lookup `NullPointerException`. I added an
`--install-from <major:minor> <operation-number>` mode to the bridge test
script so the fixed operation payload sequence can be resumed using a fresh
identity snapshot and the target's existing ESP PARTUUID. Resuming at operation
12 verified that retry and completed operations 13–16. Operation 17
(`system_ext_a` to `system_ext_b`) then stopped making progress: the target
QEMU block read/write counters stayed unchanged for over two minutes, guest
shell commands timed out, and the bridge call did not return. The VM was shut
down after its 20-minute slot. Transcript logs are
`out/pc-logs/install/install-run-20260930-permissive.log` and
`out/pc-logs/install/install-resume-20260930-permissive.log`.

This run does **not** verify a complete install or target-only boot. Slot B is
incomplete, the remaining copies/clones and loader-entry write were not
verified, and the target home screen, `_a` boot property, installer absence,
and screenshot remain outstanding. The previous VINTF diagnosis remains
withdrawn; this run confirms the live installer channel works and locates the
current blocker in target block-copy progress. User-file import remains
removed, and installed `/data` is intended to start empty. No image rebuild,
AOSP patch, or kernel change was needed for this host-side test.

## Plain VABC over UBLK conversion (2026-09-30)

The fresh 10:06 image booted in the assigned 5565 VM slot from a home-disk
copy. It reached `sys.boot_completed=1`, `ro.boot.slot_suffix=_a`, and
`ro.boot.matonos.live=1`; `ro.virtual_ab.ublk.enabled=true`, the UBLK control
device and module, `system_a`/`system_ext_a` mappings, and the Launcher3 home
screen were observed. Screenshot and serial evidence are
`/home/hanro50/matonos/vm/install/live-home.png` and
`/home/hanro50/matonos/vm/install/serial.log`. VM 5565 is stopped.

The Settings operation plan and host test plan now populate only slot `_a`;
the `_b` LP names remain zero-extent placeholders for libsnapshot to replace
during OTA. The LP writer marks VABC metadata and allows those placeholders.
Bootctrl persists `NONE`, `UNKNOWN`, `SNAPSHOTTED`, `MERGING`, and `CANCELLED`
in its redundant misc records. ODM image generation is explicitly enabled so
`odm_a` is part of the live source. Native builds and preflight pass. A full
image build has been requested; target installation and target-only boot are
still pending.

The 12:37 full image passed product config after adding `TARGET_COPY_OUT_ODM :=
odm`, but fresh live boot rebooted to EFI before ADB. The debug UKI trace
isolated the failure: `/odm` was declared EROFS in both live and installed
fstabs, while install BoardConfig made `odm.img` ext4. The area BoardConfig
now sets the ODM filesystem to `$(TARGET_RO_FILE_SYSTEM_TYPE)` (EROFS). A new
full build and fresh live boot are required before continuing to disk install.

The corrected 13:15 full build produced `odm.img` recognized by `file` as
EROFS. BT-fix's 5567 boot attempt ended at the EFI menu countdown after six
minutes, with no Android or ADB. The coordinator's 5570 VM is still active;
its serial log likewise ends at the EFI countdown and ADB remains offline.
Per the coordinator's order, no install VM is running. Fresh live-boot and
target install verification are still pending.

The later 13:15 image retry clarified the boot failure. Its debug entry reached
init, but SurfaceFlinger repeatedly aborted because `ro.hardware.egl` was
unset despite Mesa's EGL libraries being present. `/odm/etc/build.prop` in the
build output lacks the bundle properties. Enabling `PRODUCT_BUILD_ODM_IMAGE`
made AOSP overwrite the custom bundle `odm.img` staged earlier by
`tools/build.sh`. See the exact shared build integration required in
`SHARED-CHANGES.md`. VM evidence is in
`/home/hanro50/matonos/vm/install/{debug-selected-1315.log,serial-tombstone-1315.log}`;
the latter's root-shell `logcat -b crash -d` captured the tombstone.
This image did not reach `sys.boot_completed`, so the live home screen and
target install remain unverified.
