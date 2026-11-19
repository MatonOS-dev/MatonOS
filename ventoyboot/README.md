# Ventoy image-file boot

The live image's EFI entry still boots the kernel and stock AOSP ramdisks. A
final initrd overlays `/init` with a small static x86_64 NDK helper and saves
the original binary as `/init.android`. The helper mounts proc/sysfs, checks
`androidboot.boot_part_uuid` against block partition `PARTUUID`s, and execs the
saved init immediately when it is present. That is the normal dd-to-USB and
QEMU path.

If the PARTUUID is absent, the helper polls for at most nine seconds, then
loads the matching kernel's `nls_utf8`, `exfat`, and `ntfs3` modules from the
helper initrd. It mounts discoverable vfat, exFAT, and NTFS3 partitions
read-only and scans regular files to depth three. A candidate image must have
a valid GPT containing both the requested ESP PARTUUID and a GPT partition
named `super`; filename alone is not used to identify the image. It attaches
the image read-only on a loop device (no partition scan), then re-exports that
loop device as a read-only SCSI disk through the kernel's SCSI target loopback
fabric (`target_core_mod`, `target_core_iblock`, `tcm_loop`). This is needed
because stock first-stage init creates `/dev/block/by-name` links only for
partitions whose parent is an MMC, NVMe or SCSI device; a loop device is
virtual and gets none, which ends in `realpath failed: /dev/block/by-name/super`.
Only the SCSI disk's partitions carry the boot PARTUUID. The helper waits
briefly for it, detaches every scan mount (first-stage `SwitchRoot` aborts on a
rootfs mount it cannot move into `/system`), logs the result to kmsg, and execs
Android's original first-stage init even on failure.

`tools/make-live.sh` creates the shim initrd each time it repacks a live image.
The helper builds outside Soong with the configured Android NDK (`ANDROID_NDK`
or `~/Documents/android-ndk-r30`) and is included only in the live image's
ESP/initrd chain. No AOSP source patch, new SELinux policy, or kernel change is
used. The ACK 7.3 kernel has loop and configfs built in and ships the exFAT,
NTFS3, UTF-8 NLS, optical and SCSI target modules the helper loads.

## Tests

- `ventoyboot/build.sh /tmp/ventoyboot` builds a static Android x86_64 helper.
- Repack with `bash tools/make-live.sh -o <product-out> -d <image.img>`.
- For a plain image regression, boot the repacked image with
  `tools/run-qemu-live.sh -i <image.img> -g none -m 4096 -a 5556 -s <log>`.
  The serial log should contain no `matonos-ventoyboot` message on the fast
  path; `sys.boot_completed` should reach `1`.
- For image-file fallback, place the live image on an exFAT or NTFS Ventoy data
  partition and boot it from Ventoy. The serial/kmsg log should show the
  embedded PARTUUID match, loop attachment, and Android boot completion.

## Real hardware steps

1. Rebuild the live image with the coordinator and copy the `.img` file to a
   Ventoy USB's exFAT or NTFS data partition.
2. Boot the USB in UEFI mode, choose the MatonOS image in Ventoy, and wait for
   the launcher. Confirm Wi-Fi, audio, and suspend as usual.
3. Collect `dmesg | grep matonos-ventoyboot` and confirm the requested PARTUUID
   was found on the `tcm_loop` SCSI disk before first-stage init continued.
4. As a regression, write the same image directly to a USB drive and boot it;
   it should take the immediate PARTUUID path without the nine-second wait.

## Open issues

The Ventoy release's Linux installer requires privileged access to a block
device. This host requires interactive sudo authentication, so the official
installer cannot be run unattended here. QEMU validation should use a
Ventoy-installed raw disk when privileged access is available; until then, a
raw-disk image-file fixture can validate the helper's scan/loop handoff without
validating Ventoy's own UEFI menu and chainloader.

## Optical media (ISO)

`tools/make-iso.sh` wraps the live image in a UEFI ISO: the El Torito EFI
image is a copy of the live ESP, and the live `.img` sits as a file in the
ISO9660 root. A CD-ROM exposes no partitions, so the boot PARTUUID never
appears; the helper then loads `cdrom`, `sr_mod` and `isofs`, mounts `sr*`
whole devices as iso9660 and finds the image with the same PARTUUID search.
