# Persistent QEMU disk image

`tools/installer.sh --image <file> --size <bytes>` creates a sparse installed
GPT image without root or loop devices. It writes the current `super.img`,
leaves `userdata` empty for fs_mgr to format on first boot, and builds the ESP
from signed installed UKIs in the current live image. The image uses the
installed A/B fstab and enforcing SELinux profile.

The installed kernel needs both disk identifiers in its signed command line:

- `androidboot.boot_devices` tells first-stage init which physical device to
  search for the GPT partitions and create `/dev/block/by-name/*` links. For
  the runner below, QEMU puts USB storage behind xHCI at
  `pci0000:00/0000:00:01.0`.
- `androidboot.boot_part_uuid` identifies the ESP to the MatonOS boot-control
  HAL. It is the ESP GPT PARTUUID, not a substitute for `boot_devices`.

Secure Boot UKIs embed their command line. With Secure Boot enabled,
systemd-stub ignores command-line overrides from loader entries, so the
installer stamps these image-specific values into the live image's signed
installed UKIs and signs them again. The loader entries are `A.conf` and
`B.conf`, as expected by the boot-control HAL.

## Build the payload and disk image

Run from the device-tree checkout or worktree. `MATON_LIVE_IMAGE` must point
at the latest live image produced from the same product output. The kernel
input below is extracted from its installed UKI so the payload step does not
depend on the stale `prebuilt/bzImage`.

```sh
cd /mnt/data/aosp/device/maton/pc_x86_64
mkdir -p /mnt/data/aosp/out/pc-logs/disk-vm
MATON_LIVE_IMAGE=/mnt/data/aosp/out/target/product/pc_x86_64/matonos-live-x86_64.img
mcopy -i "$MATON_LIVE_IMAGE@@1048576" ::/EFI/Linux/matonos-installed-a.efi \
  /mnt/data/aosp/out/pc-logs/disk-vm/installed-a.efi
objcopy --dump-section .linux=/mnt/data/aosp/out/pc-logs/disk-vm/current-kernel \
  /mnt/data/aosp/out/pc-logs/disk-vm/installed-a.efi
PATH="$PATH:/mnt/data/aosp/out/host/linux-x86/bin" MATON_ALLOW_2011_ONLY_SHIM=1 \
  tools/make-payload.sh -S \
    -o /mnt/data/aosp/out/target/product/pc_x86_64 \
    -k /mnt/data/aosp/out/pc-logs/disk-vm/current-kernel \
    -b /mnt/data/aosp/out/pc-logs/secureboot/payload-check/efi/systemd-bootx64.efi \
    -s 6442450944 \
    -d /mnt/data/aosp/out/pc-logs/disk-vm/payload
MATON_LIVE_IMAGE="$MATON_LIVE_IMAGE" \
  PAYLOAD_DIR=/mnt/data/aosp/out/pc-logs/disk-vm/payload \
  tools/installer.sh \
    --image /home/hanro50/matonos/vm/disk-vm/disk.img \
    --size 68719476736
```

The UKIs and product images are inputs; no AOSP build is part of this flow.
The installed UKIs are extracted from the live ESP at its GPT offset, so the
`mcopy` command uses `@@1048576` for the current live image layout.

## Inspect and boot

```sh
sgdisk -v /home/hanro50/matonos/vm/disk-vm/disk.img
sgdisk -p /home/hanro50/matonos/vm/disk-vm/disk.img
mdir -i /home/hanro50/matonos/vm/disk-vm/disk.img@@1048576 ::/loader/entries
mdir -i /home/hanro50/matonos/vm/disk-vm/disk.img@@1048576 ::/EFI/Linux
```

Boot the persistent disk with the Venus-capable QEMU:

```sh
MATON_QEMU_PERSISTENT=1 \
MATON_QEMU=/home/hanro50/matonos/qemu-venus/bin/qemu-system-x86_64 \
  tools/run-qemu-live.sh \
    -i /home/hanro50/matonos/vm/disk-vm/disk.img \
    -g venus -m 12288 -a 5556 \
    -s /home/hanro50/matonos/vm/disk-vm/serial.log
```

When boot completes, verify enforcement, persistent `/data`, and the boot
property:

```sh
/mnt/data/aosp/out/host/linux-x86/bin/adb -s localhost:5556 shell \
  'getenforce; mount | grep " /data "; getprop sys.boot_completed'
/mnt/data/aosp/out/host/linux-x86/bin/adb -s localhost:5556 shell \
  'echo survives-reboot > /data/local/tmp/disk-vm-marker'
```

Stop and restart only this VM through the VM manager, then verify persistence:

```sh
/mnt/data/aosp/out/pc-logs/agents/vm.sh kill 5556
MATON_QEMU_PERSISTENT=1 \
MATON_QEMU=/home/hanro50/matonos/qemu-venus/bin/qemu-system-x86_64 \
  tools/run-qemu-live.sh \
    -i /home/hanro50/matonos/vm/disk-vm/disk.img \
    -g venus -m 12288 -a 5556 \
    -s /home/hanro50/matonos/vm/disk-vm/serial.log
/mnt/data/aosp/out/host/linux-x86/bin/adb -s localhost:5556 shell \
  'cat /data/local/tmp/disk-vm-marker; getprop sys.boot_completed'
```

## Current verification status

The file-mode image now embeds the installed A/B command lines, correct QEMU
`boot_devices`, and the ESP PARTUUID. Its signed UKIs pass signature
verification, and the loader entries point at the installed UKIs. QEMU gets
through first-stage mount and into Android startup.

The current image does not reach `sys.boot_completed`: the serial log shows
SELinux denying `matonos_driver` `{ search }` on `/dev/block` labeled
`block_device`. The boot-control HAL therefore cannot update slot state;
vold's checkpoint commit fails and Android reboots. The policy correction is
an `allow matonos_driver block_device:dir search;` rule in the existing
`sepolicy/matonos/matonos_driver.te` file; it is now in this worktree. The
current image predates that policy change. This disk-vm task forbids Soong/
image builds, so booting the updated policy and verifying persistence remain
outstanding.
