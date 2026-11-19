# Handover — night 2026-11-18 → 19

Branch `inst-layout` (worktree `~/matonos/build/inst-layout-wt`, pushed; the
server checkout follows it). Not merged to main yet: the VM install test has
not passed end to end.

## Done overnight

- **Live boot with AVB**: first-stage now creates dm nodes for logical vbmeta
  partitions before `AvbHandle::Open()`. This is folded into the approved
  `system/core/0001-map-file-backed-system-images.patch`, because preflight
  allows no new patch files. It can be split back out into an approved 0002.
  Live image boots `verifiedbootstate=green`, enforcing.
- **ESP format**: in-house FAT32 formatter (`FatVolume::Format`) replaces
  `newfs_msdos`, which vendor domains may not exec. Host fsck.fat is clean.
- **Install mount points**: made by init in the system_ext rc. vendor_init
  can write neither `/mnt` nor `/maton_system`. Init now runs `restorecon` on
  the image-store roots after mounting.
- **Image population**: mapper symlinks are resolved to validated
  `/dev/block/dm-N` nodes before the `O_NOFOLLOW` opens.
- **fiemap helper**: libfiemap SELinux rules (gsid-equivalent: sysfs_dm,
  block ioctls, FIEMAP).
- **Live dm name clash**: images are now mapped as `maton-img-<name>`. Before,
  `ImageManager` refused because the live session owns `system_a`, and a
  forced unmap could have removed the live device. Image files keep their
  real names for first-stage.

## Where it stands (updated ~11:00)

- **Install test PASSED** on build-26, merged to main (e67b64b, pushed):
  enforcing install, installed disk boots `green`, enforcing, slot `_a`,
  `ro.boot.boot_part_uuid` from EFI `LoaderDevicePartUUID`.
- Fixes since 08:30: first stage mounts the image store through a dm-linear
  node (raw-partition mount blocked the image tables with EBUSY);
  `/maton_system` root mount point; loader entries are `title`+`efi` only
  (an `options` line replaced the signed UKI cmdline and lost the pinned
  vbmeta digest); no `boot_devices` pin; SlotWriter writes option-less entries.
- Booting the installed disk in QEMU: NVMe `bootindex=1`, fresh vars.

User decisions 11-19 (memory install-foolproof-2026-11-19): no patch split;
EFI NVRAM entry; trim the SSD before reformatting; SMART health findings
(warnings + errors needing a tick-box, never refuse); clone-safe boot
(re-check image extents, duplicate PARTUUID message); faster wrapper builds.

## Next, in order

1. Installer follow-ups (user 11-19): EFI NVRAM entry + trim, SMART
   health, clone checks, build-wrapper speed.
2. (done) merge to main.
3. (done) OTA A→B→A on the **testing** channel: build-33 `_a` → 34 `_b` →
   35 `_a`, then an enforcing finalize of 35 booted `_b`. Slot writer needed
   `sys_admin` (dm table ioctls) + `kmsg_debug` append (c34337e); 33/34 lacked
   it, so those two hops ran stage/finalize with setenforce 0. Settings still
   has no stage/finalize caller; descriptor passing needs a new frozen AIDL.
4. (done, 8e97a57) Pointer capture: on build-36, LED_MISC on the MatonOS
   Pointer node switches QEMU vmmouse relative/absolute and inputd logs it.
   Still open: an app-level check (Firefox `requestPointerLock`) to confirm
   InputReader sets the LED.
5. Clean ACK 7.3 build: `build-kernel.sh` now builds the patch series as
   reproducible commits on `matonos-build` (1d48001, not pushed until the
   server kernel build passes). Kernel tree copied clean to the server at
   `/mnt/data/matonos/ack-7.3`; build there, then stage into prebuilt/.

## ISO / Ventoy (11-19 afternoon)

- ISO boot needs `ide-cd ... bootindex=1` in QEMU (`-cdrom` has no bootindex).
- Stock first-stage init links `/dev/block/by-name` only for partitions whose
  parent is MMC, NVMe or SCSI; loop devices are virtual and get none
  (`realpath failed: /dev/block/by-name/super`). The helper now re-exports the
  read-only loop as a `tcm_loop` SCSI disk (target_core_mod/iblock + tcm_loop,
  nexus before LUN link) and detaches its scan mounts before exec'ing init
  (SwitchRoot aborts on mounts it can't move). debb4a2; debug ISO booted from
  CD to boot_completed, enforcing.
- The ISO on maton.hanro50.net.za (matonos-1795085975) predates this and does
  NOT boot. Replace with one from build-37 (main 8e97a57): upload new + sha256,
  verify, delete the old pair (one ISO only).
- Debug loop for the helper: `~/matonos/vm/claude/iso/dbg/` (explicit
  linux/initrd loader entry + add.cpio.gz; `objcopy --update-section` on the
  UKI silently keeps the old size, don't use it).

## Known noise

- `mke2fs.conf` read denial (matonos_driver_helper): harmless; mke2fs
  uses its built-in defaults.
- Build log "Repository corruption encountered" is an intentional negative
  test in the subsystem suite (it passes).

## VM install recipe

Live image from server `out/target/product/pc_x86_64/matonos-live-x86_64.img`
→ `~/matonos/vm/claude/inst/live.img`; fresh 32G `target.img` as NVMe;
`tools/run-qemu-live.sh -g std -a 5557` headless. In Settings `InstallAlias`:
Choose a drive → scroll → Select drive (nvme0n1) → scroll to the end →
confirm switch → "Confirm erase and install". For the denial list in one
pass: `adb root; setenforce 0` before starting the install.

out.old (server, 70 GB) kept as asked.
