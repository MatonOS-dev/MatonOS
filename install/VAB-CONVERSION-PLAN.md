# VABC-over-UBLK conversion plan

This is a plan only. It records the target installed layout and the required
integration work; it does not convert product files, kernel config, installer
requests, or boot-control code. Keep the live image on its existing live fstab
and boot path.

## Target architecture

- Installed systems use AOSP Virtual A/B with VABC/userspace snapshots over
  UBLK. `snapuserd` stays in the generic first-stage ramdisk; `dm-user` is not
  required. Use `PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := none` for the first
  bring-up unless COW size proves too large; later choose a supported
  compression method based on measured OTA space and CPU costs.
- Installed `super` has one set of unsuffixed logical partitions: `system`,
  `system_ext`, `product`, `vendor`, `odm` (and any other enabled dynamic
  partition named by product metadata). A/B is provided by COW snapshots in
  `/data`, not by permanent `_a` and `_b` copies. Keep `addons_a` and
  `addons_b` as the separately user-decided 512 MiB per-slot partitions.
- Keep the installed boot A/B entries, `androidboot.slot_suffix`, systemd-boot
  attempt counting, and misc-backed boot selection. A/B slots select kernels
  and add-ons; both entries mount the same logical partition names and use the
  installed fstab. Live continues to use its live fstab and only its existing
  live image partition set.

## Product and board configuration changes

1. In the product makefile (`device/maton/pc_x86_64/pc_x86_64.mk` or a small
   install-owned included product fragment), inherit
   `build/make/target/product/virtual_ab_ota/vabc_features.mk`. Keep
   `PRODUCT_USE_DYNAMIC_PARTITIONS := true`; set
   `PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := none` initially. Ensure
   `RELEASE_VABC_UBLK_ENABLE_FLAG` is set by the release configuration so
   `vabc_features.mk:40–44` emits `ro.virtual_ab.ublk.enabled=true`, or add
   that property explicitly in the product if release flag wiring is not
   enabled for this tree. Verify generated payload metadata retains VABC and
   does not carry `disable_ublk`.
2. In `BoardConfig.mk` plus install-owned included fragments, enable dynamic
   partitions and configure a **single unsuffixed LP group**. Remove
   `AB_OTA_UPDATER := true`, slot-suffixed partition declarations, and any
   product variables that cause duplicated A/B LP images. Do not remove A/B
   bootloader/boot-control support: the system still has A/B boot entries and
   slot-specific kernels/add-ons.
3. Size the installed super to roughly 6 GiB (binary GiB; exact image payloads
   and OTA growth determine member sizes). Define one group, e.g.
   `pc_dynamic_partitions`, with capacity below the physical super size so LP
   metadata and alignment fit. Set unsuffixed member sizes to fit actual
   `system`, `system_ext`, `product`, `vendor`, and `odm` images with growth
   headroom. Leave intentional unallocated free extents for the first COW
   writes. libsnapshot consumes common free super extents for COW first, then
   places remaining COW backing images on `/data`; a tight super must never
   assume that every COW byte fits there. Determine final member/COW reserve
   sizes from built image sizes and representative OTA `estimate_cow_size`
   data before freezing the 6 GiB value.
4. Keep live-image construction compact: repack the current live super with
   only its unsuffixed installed logical names needed for the live boot, using
   the live build's existing size policy. The live artifact must keep booting
   unchanged. Do not require the live super to contain a second copy or an
   installed-userdata snapshot state.

## Fstab, boot, and ramdisk

1. Create/adjust separate installed and live fstabs. Installed entries mount
   unsuffixed logical partition names, use dynamic-partition flags, and have
   no `slotselect` suffix. Live retains the current live-specific fstab suffix
   and behavior. Keep userdata and metadata entries required for snapshot
   state and backing COW images; make sure first-stage init can mount userdata
   early enough for VAB.
2. Keep two installed boot entries with `androidboot.slot_suffix=_a` and
   `_b`, the appropriate slot-specific kernel/add-on selection, and the
   installed fstab selector. Both mount the same unsuffixed system partitions.
   Retain the `androidboot.matonos.live=0` marker. Live entries keep the
   current `fstab_suffix=live`, slot A and live marker.
3. Ensure the **generic ramdisk passed by systemd-boot** contains
   `snapuserd_ramdisk` as `/system/bin/snapuserd` (the ramdisk binary/symlink)
   and the `snapuserd.rc` init service definition. AOSP product comments say
   T+ VABC products put snapuserd in generic ramdisk, while vendor ramdisk
   configurations may put it elsewhere. Inspect the generated `ramdisk.img`
   and booted init service path rather than assuming `vendor_ramdisk.img` is
   used by these UKIs. The daemon must be executable before `/system` is
   mounted through the snapshot.
4. No first-stage module loader currently exists by device policy. Set
   `CONFIG_BLK_DEV_UBLK=y` in `kernel/pc.config`; the prebuilt merged kernel
   currently has `CONFIG_BLK_DEV_UBLK=m` with `ublk_drv.ko` only in the vendor
   modules bundle, which is too late for first-stage init. If built-in is
   rejected after kernel review, explicitly design and verify early ramdisk
   module loading and package the module plus dependencies in the ramdisk.
   Keep UBLK's required kernel ABI/opcode config matched to AOSP's linked
   `libublksrv`.

## Boot-control HAL and snapshot state

1. Upgrade `native/matonos-bootctrl/BootControl.cpp` so
   `getSnapshotMergeStatus()` reads the state from persistent misc records and
   `setSnapshotMergeStatus()` atomically writes the exact enum, including
   `NONE`, `UNKNOWN`, `SNAPSHOTTED`, `MERGING`, and `CANCELLED`. Preserve and
   validate the status across reboots using the existing redundant/checksummed
   record scheme. Treat `CANCELLED` as the bootloader invalidating an
   in-progress snapshot/merge; do not map all unsupported states to `NONE`.
2. Respect the stable AIDL contract: reads after writes return the set status;
   state survives reboot; writes are atomic; `MERGING` blocks operations that
   erase userdata/metadata, and active-slot changes should be refused or
   warned until safe. A bootloader-detected flash of super that invalidates
   pending snapshots reports `CANCELLED`.
3. Keep the HAL's existing slot answers coherent with boot entries:
   `getCurrentSlot()` from `androidboot.slot_suffix`, active slot from
   loader.conf/misc state, `getNumberSlots() == 2` for installed boots,
   correct slot suffix, and working `markBootSuccessful`, active, bootable,
   and unbootable operations. Live may keep its single-slot special behavior
   and no-op merge-state behavior because it will never run an OTA snapshot.
4. Individual snapshot state/progress remains managed by `libsnapshot` in
   metadata; the boot-control HAL stores the cross-reboot bootloader safety
   marker. `libsnapshot` writes `SNAPSHOTTED` before persisting an unverified
   update, `MERGING` before/while merge state is active, and clears to `NONE`
   after completion/cleanup (`snapshot.cpp:3311–3363`). Do not duplicate the
   merge engine in the HAL.

## Installer v1 operation changes

- Change `create_lp_metadata` from the two `_a` / `_b` groups to one
  unsuffixed `pc_dynamic_partitions` group and unsuffixed members with final
  sizes. Include enough free extents for initial COW and keep total group
  capacity within the declared super extent.
- Keep GPT entries `esp`, `boot`/XBOOTLDR, `misc`, `metadata`, `super`,
  `addons_a`, `addons_b`, and `userdata`; reduce super from 10.5 GiB to the
  measured ~6 GiB target. User files remain skipped and userdata starts empty.
- Keep format operations for ESP, XBOOTLDR, add-ons and userdata, plus loader
  and UKI writes. Copy each live system image **once** into its one
  unsuffixed installed logical partition.
- Drop every `clone_partition` operation that copies `_a` to `_b`; there are
  no permanent B logical copies. Keep the per-slot boot entries and
  `addons_b`, since those are still part of boot fallback and module matching.
- Keep executor validation generic. The service executes the validated
  unsuffixed LP metadata and copy operations supplied by the Settings plan;
  it must not add its own layout policy.

## Verification plan (fresh builds and boots)

1. Before implementation, capture built image member sizes, final generated
   product properties, `lpmake` metadata and generic/vendor ramdisk contents.
   Confirm VABC is enabled, userspace snapshots are enabled, UBLK property is
   true, VABC payload generation uses the chosen method, and the product
   doesn't force `disable_ublk`.
2. After configuration changes, request one coordinated full image/kernel
   build. Run preflight and inspect the generated artifacts. Confirm the
   merged kernel has UBLK built-in; check the ramdisk includes
   `/system/bin/snapuserd` and init rc; verify there is one unsuffixed LP
   group with expected free-space capacity. Never start a test VM during a
   `RUNNING` build.
3. Boot the live image in one headless QEMU VM (not port 5555); verify it
   reaches the home screen with its old live fstab and one current live
   partition set. Shut it down before subsequent builds or tests.
4. With a blank 64 GiB NVMe target attached, use the bridge-facing installer
   test script to write GPT, format, create unsuffixed LP metadata, copy each
   logical image once, and write boot files. Check GPT and LP metadata with
   `sgdisk`/`lpdump`; assert no `_a` / `_b` system LPs and no clone operations.
5. Power off and boot the target disk alone. Verify the target reaches
   `sys.boot_completed=1` and the launcher home screen; check
   `ro.boot.slot_suffix`, current/active boot-control slots, all unsuffixed
   logical mounts, fresh empty `/data`, and that the installer service and UI
   are absent (`ro.boot.matonos.live=0`).
6. Exercise update/merge on a test payload: verify payload metadata is VABC;
   update_engine writes COW snapshots; logs/properties show snapuserd in UBLK
   mode and no dm-user device path; first-stage starts snapuserd before
   snapshot logical mapping; the new boot is initially unverified; successful
   slot marking triggers merge; merge progresses across reboot and clears the
   HAL marker to `NONE`. Verify rollback if the first target-slot boot is
   deliberately failed. Capture boot logs, update_engine/libsnapshot logs,
   boot-control status, partition metadata and installed-home screenshot.
7. Repeat smoke tests on the documented Ryzen/RX 6600/Intel 7265,
   Surface Pro 3/Marvell, and HP ProDesk 600 G1/Haswell profiles. Confirm the
   no-UBLK hardware case is irrelevant (UBLK is a generic kernel feature),
   while absent optional peripherals never block boot.

## Open sizing/integration checks

- Final 6 GiB super member sizes and OTA COW headroom must be measured; if an
  update's `estimate_cow_size` plus overhead exceeds super free space and
  userdata budget, the OTA must fail safely before switching slots.
- Verify which ramdisk artifact actually enters the signed UKI today and
  adjust the installer payload construction accordingly.
- Make the UBLK driver built in or add a demonstrably early module load path.
- Implement and validate boot-control snapshot merge persistence before
  enabling update_engine updates. This is required for safe rollback and
  interrupted-merge recovery.
