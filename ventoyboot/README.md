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
the image read-only with loop partition scanning enabled, waits briefly for
the requested PARTUUID, logs the result to kmsg, and execs Android's original
first-stage init even on failure.

`tools/make-live.sh` creates the shim initrd each time it repacks a live image.
The helper builds outside Soong with the configured Android NDK (`ANDROID_NDK`
or `~/Documents/android-ndk-r30`) and is included only in the live image's
ESP/initrd chain. No AOSP source patch, new SELinux policy, or kernel change is
used. The currently staged 7.2.7 kernel has loop built in and ships the exFAT,
NTFS3, and UTF-8 NLS modules.

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
   was found on a loop partition before first-stage init continued.
4. As a regression, write the same image directly to a USB drive and boot it;
   it should take the immediate PARTUUID path without the nine-second wait.

## Open issues

The Ventoy release's Linux installer requires privileged access to a block
device. This host requires interactive sudo authentication, so the official
installer cannot be run unattended here. QEMU validation should use a
Ventoy-installed raw disk when privileged access is available; until then, a
raw-disk image-file fixture can validate the helper's scan/loop handoff without
validating Ventoy's own UEFI menu and chainloader.
