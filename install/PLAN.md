# File-backed install plan

## GPT profile

The Settings app owns layout policy. The installer service validates and runs
each primitive against the confirmed physical disk; it does not choose a disk,
slot, or partition layout.

| # | Name | Size | Purpose |
|---:|---|---:|---|
| 1 | `esp` | 2 GiB | FAT32 EFI system partition with fallback loader, systemd-boot, one installed UKI and one entry. |
| 2 | `maton_system` | 16 GiB | ext4 container for libfiemap-backed EROFS images. |
| 3 | `metadata` | 64 MiB | ext4 libfiemap extent metadata and Android metadata. |
| 4 | `userdata` | Remainder | F2FS when the selected disk reports non-rotational; ext4 when rotational. |

There is no physical `super`, boot, misc, add-on, or B slot in this profile.
The live source uses A images; a fresh install writes only the `_a` logical
image names. Slot selection and OTA writes are outside this task.

## Install sequence

1. Re-enumerate the selected disk and verify its stable identity, capacity,
   in-use state and safety before each destructive operation.
2. Write GPT with the four partitions above.
3. Format the ESP as FAT32, metadata as ext4, and userdata as the selected
   ext4/F2FS filesystem.
4. Open and verify the target `maton_system` and `metadata` block devices.
   Create a dm-linear mapping covering the entire `maton_system` partition
   from sector zero, format ext4 through that mapping, then mount both ext4
   filesystems under `/mnt/matonos-install/`.
5. Call `ImageManager::Open("/mnt/matonos-install/metadata/maton/images",
   "/mnt/matonos-install/system/images")`. Create readonly backing images for
   the five A-slot system partitions plus `vbmeta_a` and `vbmeta_b`, map each,
   copy the complete live mapper image (the root vbmeta source supplies both
   slot files), fsync and read back the bytes, then unmap it.
6. Unmount the target filesystems and remove the temporary dm-linear device.
7. Write the fallback loader, installed UKI and one loader entry. The entry
   selects `_a`, carries the target ESP PARTUUID and the selected disk's
   firmware boot-device path, and marks the image as installed.

The ext4 target filesystems must be mounted through the full-disk dm-linear
wrapper because libfiemap records the leaf block device and expects the
backing partition to start at sector zero. Formatting/mounting directly from
the GPT partition offset would create an incompatible extent map.

`tools/make-payload.sh` packages the five built images individually. The live
Settings flow currently copies its live `_a` mapper sources directly; it does
not consume the compressed host payload.

## Scope and verification

The current planner always writes a fresh four-partition layout and erases
the selected disk. It does not yet detect an existing MatonOS installation or
offer a distinct repair action. It has no slot chooser and does not implement
an image downgrade check. Those behaviors need to be completed before this
can be presented as a finished repair/fresh-install workflow.

See `README.md` for build evidence and `SHARED-CHANGES.md` for the required
fstab and first-stage policy integration. No VM or installed-target boot was
performed for this change, so the layout is not yet verified end to end.

## Real hardware validation after integration

1. On the Ryzen 5800X / RX 6600 PC, inspect the selected disk and rotational
   state, install only to a disposable disk, then check GPT names, sizes and
   filesystem types.
2. Inspect `/metadata/maton/images`, `/maton_system/images`, libfiemap's
   partition metadata, mapped `_a` names and EROFS hashes before first boot.
3. Cold boot the target disk alone. Confirm first-stage mapping reaches
   `sys.boot_completed=1` and `/data` mounts using the selected filesystem.
4. Repeat on the Surface Pro 3 and HP ProDesk 600 G1, including a machine
   without optional Wi-Fi/Bluetooth hardware and a rotational userdata disk.
