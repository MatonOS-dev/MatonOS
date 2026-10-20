# Persistent QEMU disk image

`tools/installer.sh --image <file> --size <bytes>` builds an installed-mode GPT
disk image without root or loop devices. It uses the same ESP/misc/metadata/
super/userdata partition table as block-device installation. The image is
sparse; the ESP is populated through mtools at its GPT byte offset, `super`,
`misc` and `metadata` are written in place, and `userdata` is left empty for
first-boot formatting by fs_mgr.

The generated boot entries include the ESP PARTUUID as
`androidboot.boot_part_uuid`, and the QEMU q35/xHCI boot path as
`androidboot.boot_devices=pci0000:00/0000:00:1d.0`. Keep the image attached
through `tools/run-qemu-live.sh`, which attaches it as USB storage. The runner
must use persistent mode because the installed VM needs writes to userdata.

## Build the current test image

The product artifacts are read from `/mnt/data/aosp/out/target/product/pc_x86_64`.
This checkout's current output does not contain a standalone bzImage; the
latest staged kernel is at
`/mnt/data/aosp/device/maton/pc_x86_64/prebuilt/bzImage`. The systemd-boot
binary from the existing payload preparation is at
`/mnt/data/aosp/out/pc-logs/secureboot/payload-check/efi/systemd-bootx64.efi`.
These are input-only; neither is modified. AOSP's host `simg2img` is needed
when the product `super.img` is sparse.

```sh
cd /mnt/data/aosp/device/maton/pc_x86_64
PATH=/mnt/data/aosp/out/host/linux-x86/bin:$PATH \
  tools/make-payload.sh \
    -o /mnt/data/aosp/out/target/product/pc_x86_64 \
    -k /mnt/data/aosp/device/maton/pc_x86_64/prebuilt/bzImage \
    -b /mnt/data/aosp/out/pc-logs/secureboot/payload-check/efi/systemd-bootx64.efi \
    -s 6442450944 \
    -d /mnt/data/aosp/out/pc-logs/disk-vm/payload

PAYLOAD_DIR=/mnt/data/aosp/out/pc-logs/disk-vm/payload \
  tools/installer.sh \
    --image /home/hanro50/matonos/vm/disk-vm/disk.img \
    --size 68719476736
```

The second command creates a 64 GiB sparse image. Replace the `-k` input with
the staged kernel from the build you intend to test when it changes. The
installer's block-device mode remains the normal root-required interactive
installer.

## Verify and boot

Verify the GPT and FAT filesystem without mounting or booting:

```sh
sgdisk -v /home/hanro50/matonos/vm/disk-vm/disk.img
sgdisk -p /home/hanro50/matonos/vm/disk-vm/disk.img
mdir -i /home/hanro50/matonos/vm/disk-vm/disk.img@@1048576 ::/loader/entries
mdir -i /home/hanro50/matonos/vm/disk-vm/disk.img@@1048576 ::/EFI/BOOT
mdir -i /home/hanro50/matonos/vm/disk-vm/disk.img@@1048576 ::/android
```

Boot it with the current QEMU runner and the host's Venus-capable QEMU:

```sh
MATON_QEMU_PERSISTENT=1 \
  tools/run-qemu-live.sh \
    -i /home/hanro50/matonos/vm/disk-vm/disk.img \
    -g venus -m 4096 -a 5556 \
    -s /mnt/data/aosp/out/pc-logs/disk-vm/serial.log
```

The image path is attached unchanged as QEMU USB storage. The disk's ESP
PARTUUID in `loader/entries/android.conf` must match the ESP PARTUUID shown by
`sgdisk -i 1`; the boot itself is intentionally left to the coordinator/user
when the shared VM slot is available.
