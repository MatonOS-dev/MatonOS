# VABC-over-UBLK conversion plan

Implementation is in progress. The 2026-09-30 image has verified the live
UBLK/snapuserd boot path. Installer planning reserves six GiB for super,
keeps slot-suffixed LP entries with populated `_a` members and zero-extent
`_b` placeholders, and omits the `_a` to `_b` clone operations. The service
must write `LP_HEADER_FLAG_VIRTUAL_AB_DEVICE`, preserve zero-extent B names,
and validate allocated extents rather than summing future group capacities.
The boot-control implementation is being extended to persist merge status.
The live image keeps its separate live fstab and boot path.

## Target architecture

- Installed systems use AOSP Virtual A/B with VABC/userspace snapshots over
  UBLK. `snapuserd` stays in the generic first-stage ramdisk; `dm-user` is not
  required. Use `PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := none` for the first
  bring-up unless COW size proves too large; later choose a supported
  compression method based on measured OTA space and CPU costs.
- Installed `super` initially has one populated set of slot-suffixed logical
  partitions: `system_a`, `system_ext_a`, `product_a`, `vendor_a`, `odm_a`
  (and any other enabled dynamic member with `_a`). The `_b` group and members
  are created as target metadata and COW snapshots during an OTA; the
  installer does not clone full `_a` images into permanent `_b` copies. Keep
  `addons_a` and
  `addons_b` as the separately user-decided 512 MiB per-slot partitions.
- Keep the installed boot A/B entries, `androidboot.slot_suffix`, systemd-boot
  attempt counting, and misc-backed boot selection. The installed fstab retains
  `slotselect`, so A mounts `_a` and B mounts snapshot-backed `_b` after OTA.
  An initial B entry needs an explicit policy: it cannot boot before an OTA
  creates B logical metadata. Live keeps its separate fstab and populated A
  set. Source: `snapshot_metadata_updater.cpp:39–65` appends `_b` to target
  group/member names, `snapshot.cpp:3491–3504` reads separate slot metadata,
  and `libfstab/slotselect.cpp:55–74` appends the current boot suffix.

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
2. In `BoardConfig.mk` plus install-owned included fragments, retain
   `AB_OTA_UPDATER := true`, dynamic partitions, two LP metadata slots, and
   the base group/member declarations that the AOSP build suffixes by slot.
   Change the installed physical super capacity to roughly 6 GiB. Arrange
   installed LP metadata with only `_a` members populated initially; reserve
   an empty B metadata slot for the OTA target. Do not allocate full `_b`
   extents at installation.
3. Size the installed super to roughly 6 GiB (binary GiB; exact image payloads
   and OTA growth determine member sizes). Define a base group such as
   `pc_dynamic_partitions` with suffix-aware A/B LP metadata. The populated
   `pc_dynamic_partitions_a` group capacity must stay below physical super
   size after metadata and alignment. Size `_a` members to fit actual
   `system`, `system_ext`, `product`, `vendor`, and `odm` images with growth
   headroom. Leave intentional unallocated free extents for the first COW
   writes. libsnapshot consumes common free super extents for COW first, then
   places remaining COW backing images on `/data`; a tight super must never
   assume that every COW byte fits there. Determine final member/COW reserve
   sizes from built image sizes and representative OTA `estimate_cow_size`
   data before freezing the 6 GiB value.
4. Keep live-image construction compact: repack the current live super with
   only its `_a` logical names needed for the live boot, using
   the live build's existing size policy. The live artifact must keep booting
   unchanged. Do not require the live super to contain a second copy or an
   installed-userdata snapshot state.

## Fstab, boot, and ramdisk

1. Keep separate installed and live fstabs. Installed entries use base names
   plus `logical,slotselect,first_stage_mount`; fs_mgr resolves them to `_a`
   or `_b` using `androidboot.slot_suffix`. Live retains its current fstab suffix
   and behavior. Keep userdata and metadata entries required for snapshot
   state and backing COW images; make sure first-stage init can mount userdata
   early enough for VAB.
2. Keep two installed boot entries with `androidboot.slot_suffix=_a` and
   `_b`, the appropriate slot-specific kernel/add-on selection, and the
   installed fstab selector. B must not become bootable until its OTA snapshot
   metadata exists; before then A is the sole bootable installed slot.
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
4. First-stage init already loads `/lib/modules/.../modules.load` before
   mounting partitions (`first_stage_init.cpp:190–295,453–476`). Put the
   existing `ublk_drv.ko` in the generic ramdisk with
   `BOARD_GENERIC_RAMDISK_KERNEL_MODULES` and its load list. The merged kernel
   currently has `CONFIG_BLK_DEV_UBLK=m`; the module has no listed dependencies.
   Verify `/dev/ublk-control` exists before snapshot mapping. A future kernel
   rebuild may set `CONFIG_BLK_DEV_UBLK=y` instead. Keep UBLK's required
   kernel ABI/opcode config matched to AOSP's linked `libublksrv`.

## Boot-control HAL and snapshot state

1. `native/matonos-bootctrl/BootControl.cpp` now reads and persists the exact
   status enum (`NONE`, `UNKNOWN`, `SNAPSHOTTED`, `MERGING`, `CANCELLED`) in
   alternating checksummed misc records beyond the first 4 KiB reserved for
   Android bootloader control. It validates enum values and defaults only slot
   A as bootable. Runtime interaction with update_engine remains to be tested.
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
4. The HAL refuses active-slot changes while `MERGING`. Individual snapshot state/progress remains managed by `libsnapshot` in
   metadata; the boot-control HAL stores the cross-reboot bootloader safety
   marker. `libsnapshot` writes `SNAPSHOTTED` before persisting an unverified
   update, `MERGING` before/while merge state is active, and clears to `NONE`
   after completion/cleanup (`snapshot.cpp:3311–3363`). Do not duplicate the
   merge engine in the HAL.

## Installer v1 operation changes

- Change `create_lp_metadata` from two fully populated `_a` / `_b` groups to
  one populated `_a` group and no allocated `_b` members. Keep multiple LP
  metadata slots so the updater can create the target `_b` metadata and COWs.
  Include free extents for COW and keep the A group within super capacity.
- Keep GPT entries `esp`, `boot`/XBOOTLDR, `misc`, `metadata`, `super`,
  `addons_a`, `addons_b`, and `userdata`; reduce super from 10.5 GiB to the
  measured ~6 GiB target. User files remain skipped and userdata starts empty.
- Keep format operations for ESP, XBOOTLDR, add-ons and userdata, plus loader
  and UKI writes. Copy each live `_a` system image **once** into its installed
  `_a` logical partition.
- Drop every `clone_partition` operation that copies `_a` to `_b`; there are
  no permanent B logical copies. Keep the per-slot boot entries and
  `addons_b`, since those are still part of boot fallback and module matching.
- Keep executor validation generic. The service executes the validated
  slot-suffixed LP metadata and copy operations supplied by the Settings plan;
  it must not add its own layout policy.

## Verification plan (fresh builds and boots)

1. Before implementation, capture built image member sizes, final generated
   product properties, `lpmake` metadata and generic/vendor ramdisk contents.
   Confirm VABC is enabled, userspace snapshots are enabled, UBLK property is
   true, VABC payload generation uses the chosen method, and the product
   doesn't force `disable_ublk`.
2. After configuration changes, request one coordinated full image build.
   Run preflight and inspect the generated artifacts. Confirm the generic
   ramdisk contains UBLK's module and load list plus `/system/bin/snapuserd`
   and init rc; verify only `_a` LP members are populated and free-space
   capacity matches the plan. Never start a test VM during a
   `RUNNING` build.
3. Boot the live image in one headless QEMU VM (not port 5555); verify it
   reaches the home screen with its old live fstab and one current live
   partition set. Shut it down before subsequent builds or tests.
4. With a blank 64 GiB NVMe target attached, use the bridge-facing installer
  test script to write GPT, format, create slot-suffixed LP metadata, copy each
   logical image once, and write boot files. Check GPT and LP metadata with
   `sgdisk`/`lpdump`; assert populated `_a` members, no allocated `_b` members,
   and no clone operations.
5. Power off and boot the target disk alone. Verify the target reaches
   `sys.boot_completed=1` and the launcher home screen; check
  `ro.boot.slot_suffix`, current/active boot-control slots, the populated
  `_a` logical mounts, zero-extent `_b` metadata placeholders, fresh empty `/data`, and that the installer service and UI
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
- Verify first-stage init loads ramdisk `ublk_drv.ko` before snapshot mapping.
- Validate boot-control snapshot merge persistence before enabling
  update_engine updates. This is required for safe rollback and
  interrupted-merge recovery.
