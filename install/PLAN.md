# Installer v1 plan

The app owns layout policy and operation order. The install service executes one
validated operation at a time and never chooses a disk or layout. Its bridge
channel rechecks target identity, live mode, mounted state, bounds, and paths
before each write. A failure stops the plan; installing erases the selected
disk.

## Product profile

The installed system uses A/B logical partitions in dynamic super. The live
image contains only slot A and boots with its existing live-specific fstab
suffix, slot A, and the live marker. The installed image has slot-select fstab
entries and signed A/B UKIs with installed fstab suffix, their respective slot
suffix, and live marker zero. The installer creates ESP, XBOOTLDR, misc,
metadata, 10.5 GiB super, 512 MiB addons_a, 512 MiB addons_b, and userdata.
Super contains two 5 GiB groups with system, system_ext, product, vendor, and
odm members. It copies each live A member to target A and clones A to B.

The ESP carries the fallback EFI path, systemd-boot, both installed UKIs,
loader.conf, and boot-counted A/B entry files. systemd-boot counts attempts by
renaming entry files. The MatonOS boot-control HAL persists state in misc,
selects the loader default, marks the successful entry, and disables an
unbootable entry. The AOSP AIDL default HAL was checked first; its generic
boot-control implementation persists Android bootloader-message state but
does not select systemd-boot entries, so a small PC-specific implementation is
needed. It finds ESP and misc on the same physical disk as the
currently mounted logical system partition; installed UKIs therefore do not
need a build-time target PARTUUID. Firmware NVRAM is not modified.

User files are skipped by decision: the target has no staging partition or
import service, and its `/data` is initialized empty on first boot with new FBE
keys.

## App operation order

The Settings plan sends these independent requests through the install bridge:

1. Write the user-confirmed GPT table.
2. Format ESP, XBOOTLDR, add-on A/B, and userdata.
3. Write three-slot LP metadata for both groups.
4. Copy the live slot-A system partitions to installed slot A.
5. Write the installed A/B UKIs and select initial boot-counted entry A.
6. Clone every A logical partition to its B counterpart.
7. Write the B boot-counted entry and complete loader files.

The live fstab remains selected on the source image. Installed UKIs carry
ro.boot.matonos.live=0; init starts the installer daemon only for live=1, and
the Settings Install activity alias is hidden unless the live property is 1.
There is no installed-image marker operation or user-file importer. The
installer daemon starts only for live boots, and the Settings Install activity
alias is hidden on installed boots.

## Implementation and verification state

The operation service, stable install channel, Settings request plan, A/B
BoardConfig/fstab conversion, NDK boot-control HAL, and signed UKI packager
have been implemented. Earlier QEMU install operations completed GPT, format, LP
metadata, copy and clone operations on the 64 GiB target. A target-only boot
found the ESP and started slot A's UKI, but first-stage init failed to resolve
its default fstab. The per-entry hardware/fstab/slot/boot-device options and
the fstab configuration now needs verification in a fresh image, including
home-screen boot.

## Real hardware test checklist

- Ryzen 5800X / RX 6600 / Intel 7265: identify the selected internal disk by
  model and serial; install to a blank disk and boot it alone.
- Surface Pro 3: verify the Marvell device is not selected as the target by
  mistake; install to a blank external disk and verify A/B boot.
- HP ProDesk 600 G1: verify boot and installation without optional network
  devices. Installer or boot-control failures must not block boot.
