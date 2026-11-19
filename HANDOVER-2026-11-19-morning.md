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

## Where it stands (updated ~08:30)

- Install **completes** in the VM (permissive run on build-22; the only
  enforcing denial was the daemon's dm write, fixed in ce2057a).
- Installed disk then panicked in first stage: libfiemap stored the
  store's kernel name `nvme0n1p2`; first stage waits by GPT name. Fixed in
  0eb4e25 (helper `finalize` renames it to `maton_system`), build-24.
- **Gap**: installer writes no EFI NVRAM boot entry; it relies on the
  `\EFI\BOOT\BOOTX64.EFI` fallback. In QEMU, give the NVMe `bootindex=1`
  and fresh vars. Real firmware without fallback scanning won't boot it.
- Booting the installed disk needs the same PCI topology as install time:
  the loader entry pins `androidboot.boot_devices` (partuuid wins anyway).

Earlier state:

Install flow in the VM: GPT → ESP → image store (mkfs, mount, label) →
image creation all pass enforcing. The last run (build-21) failed mapping
the images: fs_mgr looked for the target partition under
`/dev/block/by-name`. Fixed in 9e3b702 (build-22).

## Next, in order

1. Build-22 → VM install (recipe below) → boot the installed disk: check
   `ro.boot.verifiedbootstate`, enforcing, `/maton_system/ota-staging` exists.
2. Merge `inst-layout` → main, push.
3. OTA A→B→A via the live server's **testing** channel. Note from
   `ota-device` report: Settings has no stage/finalize caller yet, and
   descriptor passing needs a new frozen AIDL version.
4. Pointer capture (`wip/pointer-capture` 6a791f9): VM-test Firefox
   `requestPointerLock`, then merge.
5. ACK 7.3 is already the running kernel (7.3.0-g8bf243c0128e-dirty); the
   remaining work is a clean (non-dirty) build.

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
