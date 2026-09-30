# Plain Virtual A/B feasibility in this AOSP checkout

**Verdict: the requested no-compression, no-snapuserd Virtual A/B OTA path is
not supported end to end by the checked-in AOSP update stack.** `launch.mk`
sets the product-level Virtual A/B flag, but the current `libsnapshot` update
creation path rejects the legacy dm-snapshot mode. The product configuration
that sets VABC compression to `none` is a different path: it still uses
userspace snapshots and `snapuserd`. Do not convert the installer layout on
the basis of `launch.mk` alone. This report is source inspection only; no
layout or code was changed for this feasibility check.

## Product makefiles

- [`build/make/target/product/virtual_ab_ota/launch.mk`](../../../../build/make/target/product/virtual_ab_ota/launch.mk#L17)
  lines 17–21 sets `PRODUCT_VIRTUAL_AB_OTA := true`, the
  `ro.virtual_ab.enabled=true` property and `e2fsck_ramdisk`. This file does
  not configure the current snapshot backend or provide an OTA implementation.
- [`build/make/target/product/virtual_ab_ota/vabc_features.mk`](../../../../build/make/target/product/virtual_ab_ota/vabc_features.mk#L16)
  lines 16–20 describes baseline userspace-merge features and notes that
  `snapuserd` is expected in generic ramdisk. Lines 28–34 enable VABC and
  userspace snapshots. Lines 81–84 set the default compression method to
  `none` and add the `snapuserd` package. Therefore “compression method none”
  means uncompressed VABC COW, not kernel dm-snapshot without snapuserd.

## libsnapshot and update_engine

- [`system/fs/fs_mgr/libsnapshot/snapshot.cpp`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L3528)
  lines 3528–3563 show the decisive blocker in
  `SnapshotManager::CreateUpdateSnapshots`: when the payload lacks VABC
  metadata, it disables userspace snapshots and legacy compression
  (3530–3547), then logs `Using legacy Virtual A/B (dm-snapshot)` and returns
  `Return::Error()` (3559–3563). The later log at lines 3691–3695 that appears
  to name dm-snapshot is unreachable for this non-VABC update because of that
  earlier return.
- The same implementation’s `MapUpdateSnapshot` at
  [`snapshot.cpp:3950`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L3950)
  still contains a non-snapuserd mapping branch, but `OpenSnapshotWriter`
  rejects non-snapuserd snapshots at lines 3979–4023. This is not a complete
  update-write/merge pipeline. In particular, do not mistake the mapping
  helper for end-to-end plain VAB support.
- [`system/update_engine/payload_generator/payload_generation_config.cc`](../../../../system/update_engine/payload_generator/payload_generation_config.cc#L188)
  lines 188–215 set `vabc_enabled` from the `virtual_ab_compression` build
  setting and attach VABC COW metadata and a compression method. The method
  can be `none`, but VABC remains enabled. In the consumer,
  [`delta_performer.cc:645`](../../../../system/update_engine/payload_consumer/delta_performer.cc#L645)
  onward handles `vabc_none` by setting the COW compression parameter to
  `none`; lines 702–703 only clear VABC for `disable_vabc`. These options do
  not create the rejected legacy dm-snapshot path.
- [`system/update_engine/payload_consumer/vabc_partition_writer.cc`](../../../../system/update_engine/payload_consumer/vabc_partition_writer.cc#L60)
  uses `ICowWriter` to produce COW operations. When the
  `ro.virtual_ab.userspace.snapshots.enabled` property is false, its copy-op
  code still writes COW records (lines 79–101 and 145–168); it is not a
  replacement kernel dm-snapshot OTA writer.

## First-stage mapping and snapuserd dependency

- [`system/core/init/first_stage_mount_android.cpp`](../../../../system/core/init/first_stage_mount_android.cpp#L156)
  lines 156–182 invoke `SnapshotManager` when snapshot state requires it.
  `CreateSnapshotPartitions` (185–220) asks `IsSnapuserdRequired()` and only
  launches first-stage snapuserd when true (192–197), then calls
  `CreateLogicalAndSnapshotPartitions` (214). This means first-stage code has
  conditional snapuserd launching; it does not cure `libsnapshot`’s rejected
  update creation path.
- The snapshot driver helper at
  [`snapshot.cpp:144`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L144)
  selects UBLK or DM_USER for the current userspace snapshot backend. The
  dm-user mapping implementation is visible at lines 763–784. There is no
  active, complete dm-snapshot update mapping path exposed by the examined
  update writer flow.

## Kernel support required and currently available

Kernel prerequisites for a kernel dm-snapshot backend include device mapper
core (`CONFIG_BLK_DEV_DM`), `CONFIG_DM_SNAPSHOT`, and its buffer/prison support
(`CONFIG_DM_BUFIO`, `CONFIG_DM_BIO_PRISON`). `CONFIG_DM_PERSISTENT_DATA` is
also enabled in this kernel fragment, though it is for thin provisioning,
not the dm-snapshot target itself.

- [`kernel/base.config`](../kernel/base.config#L3205) lines 3205–3216 has
  `CONFIG_BLK_DEV_DM_BUILTIN=y`, `CONFIG_DM_BUFIO=m`,
  `CONFIG_DM_BIO_PRISON=m`, `CONFIG_DM_PERSISTENT_DATA=m`, and
  `CONFIG_DM_SNAPSHOT=m`.
- [`kernel/pc.config`](../kernel/pc.config#L79) sets `CONFIG_BLK_DEV_DM=y`.
  The checked-in merged config [`prebuilt/kernel.config`](../prebuilt/kernel.config#L3205)
  has device mapper core, `DM_BUFIO`, and `DM_SNAPSHOT` built in, so a cold
  first-stage dm-snapshot target would not need those as modules. This differs
  from the editable base fragment; the built kernel configuration is what
  ships.
- The packaged `prebuilt/modules/` inventory includes `dm-bio-prison.ko` but
  has no `dm-snapshot.ko` or `dm-bufio.ko`. The currently merged prebuilt
  kernel config has the target and buffer support built in, so this inventory
  is not by itself a blocker for that exact kernel artifact. Keep the actual
  merged config and runtime kernel/module set aligned if the kernel is rebuilt.
- No `CONFIG_DM_USER` entry was found in `kernel/base.config` or the merged
  `prebuilt/kernel.config`. This matches the no-dm-user intent, but is also
  why the VABC userspace-snapshot option described above cannot be selected
  without a kernel/userspace backend change. `CONFIG_DM_LOG_USERSPACE` is a
  separate target and is not `dm-user`.

## Boot-control HAL merge contract

The stable AIDL contract is in
[`hardware/interfaces/boot/aidl/android/hardware/boot/IBootControl.aidl`](../../../../hardware/interfaces/boot/aidl/android/hardware/boot/IBootControl.aidl#L60)
and the status enum in
[`MergeStatus.aidl`](../../../../hardware/interfaces/boot/aidl/android/hardware/boot/MergeStatus.aidl#L21).
For Virtual A/B, the HAL must persistently and atomically implement
`getSnapshotMergeStatus()` and `setSnapshotMergeStatus()` across reboot;
`MERGING` also constrains bootloader operations (IBootControl lines 128–157).
The states are `NONE`, `UNKNOWN`, `SNAPSHOTTED`, `MERGING`, and `CANCELLED`.

`libsnapshot` owns the transitions: in
[`snapshot.cpp:3311`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L3311)
lines 3322–3362 map update state `Unverified` to `SNAPSHOTTED`, `Merging` or
`MergeFailed` to `MERGING`, and the clear/completed states to `NONE`. It writes
the hazardous states to boot control before persisting local state, and clears
them after local state is safely updated. The HAL should therefore store and
return the exact last requested state (and report `CANCELLED` if the
bootloader invalidated a pending merge); it should not try to orchestrate the
merge itself. `markBootSuccessful()` and slot state are a separate boot-control
contract that enables the successful-slot/update flow.

The current project HAL is not ready for this contract:
[`native/matonos-bootctrl/BootControl.cpp:348`](../native/matonos-bootctrl/BootControl.cpp#L348)
always returns `NONE`, and lines 455–462 reject every requested state except
`NONE`, even though its persistent state record has a `merge_status` field.
This would need implementation before any VAB mode could safely use the
existing HAL.

## Conclusion

The build system has a *product flag* for Virtual A/B, the kernel artifact has
the core dm-snapshot target, and first-stage init has generic snapshot-mapping
hooks. But the OTA write path in this AOSP revision explicitly rejects the
legacy dm-snapshot mode. The available uncompressed VABC path still depends on
snapuserd/userspace snapshots and is not plain kernel dm-snapshot. Achieving
the requested mode would need changes to AOSP `libsnapshot`/update-engine
snapshot creation, COW writing, mapping, and merge behavior, plus merge-state
persistence in the project boot-control HAL. No such changes were made here.

## Addendum: VABC over UBLK

**Updated feasibility: the checked-in AOSP stack supports Virtual A/B with
VABC over UBLK, without the dm-user kernel target.** This is the supported
replacement direction recorded in `NOTES.md`; it retains `snapuserd` in the
generic ramdisk. It is a userspace snapshot design backed by kernel UBLK, not
the previously requested no-snapuserd dm-snapshot design. No conversion was
made while preparing this addendum.

### Payload/update writer

- [`build/make/target/product/virtual_ab_ota/vabc_features.mk`](../../../../build/make/target/product/virtual_ab_ota/vabc_features.mk#L28)
  lines 28–34 enable Virtual A/B, VABC and userspace snapshots. Lines 40–44
  add `ro.virtual_ab.ublk.enabled?=true` when the product release flag
  `RELEASE_VABC_UBLK_ENABLE_FLAG` is nonempty. The product must inherit this
  feature file and ensure the release flag is set (or explicitly set the
  property true for the product configuration). Lines 81–84 select the
  default no-compression method and package `snapuserd`; compression may
  instead use a supported algorithm as selected by the OTA/build metadata.
- Payload generation writes the VABC feature and compression metadata in
  [`system/update_engine/payload_generator/payload_generation_config.cc`](../../../../system/update_engine/payload_generator/payload_generation_config.cc#L188)
  lines 188–215. The update consumer routes partition update operations to
  `VABCPartitionWriter`, which writes COPY/REPLACE/DIFF and merge-sequence COW
  records using `ICowWriter` ([`vabc_partition_writer.cc`](../../../../system/update_engine/payload_consumer/vabc_partition_writer.cc#L80),
  lines 80–95, 177–204, 278–345). `vabc_none` changes the COW compression
  parameter and COW size estimate but leaves `vabc_enabled` set
  ([`delta_performer.cc:645`](../../../../system/update_engine/payload_consumer/delta_performer.cc#L645),
  lines 645–704). Thus a no-compression VABC OTA is an available conservative
  first profile; it still uses VABC COW operations, libsnapshot and snapuserd.
- [`snapshot.cpp`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L3528)
  lines 3528–3563 accepts the VABC manifest and sets `using_snapuserd`;
  lines 3683–3690 set the snapshot backend according to `IsUblkEnabled()`
  unless the manifest explicitly disables UBLK. COW allocation uses available
  free extents in super first and places the remainder in a userdata-backed
  image: [`partition_cow_creator.cpp`](../../../../system/fs/fs_mgr/libsnapshot/partition_cow_creator.cpp#L173)
  lines 173–215 computes the split, and `snapshot.cpp` lines 3821–3848 creates
  COW logical partitions from free super space. This confirms tight super is
  feasible as an allocation policy, but its COW reserve/headroom must be sized
  against OTA estimates; COW overflow fails the update.

### UBLK server and first-stage path

- [`system/fs/fs_mgr/libsnapshot/snapuserd/ublk_block_server.cpp`](../../../../system/fs/fs_mgr/libsnapshot/snapuserd/ublk_block_server.cpp#L26)
  lines 26–88 registers an `android_snapshot` UBLK target. Lines 90–133
  initialize the UBLK device/control node; lines 198–235 serve block reads by
  calling the snapshot delegate. The build includes this source and links
  `libublksrv` for Android targets in
  [`snapuserd/Android.bp`](../../../../system/fs/fs_mgr/libsnapshot/snapuserd/Android.bp#L96)
  lines 96–110 and 124–160.
- [`system/core/init/first_stage_mount_android.cpp`](../../../../system/core/init/first_stage_mount_android.cpp#L185)
  lines 185–220 first ensures userdata is available for COW images, asks
  `IsSnapuserdRequired()`, reads `UpdateUsesUblk()`, and launches the
  first-stage snapuserd in UBLK mode before creating snapshot logical
  partitions. It also recreates UBLK and device-mapper nodes via the uevent
  callback (199–212).
- For UBLK mapping,
  [`snapshot.cpp`](../../../../system/fs/fs_mgr/libsnapshot/snapshot.cpp#L840)
  lines 840–887 ensures snapuserd is in UBLK mode, creates the UBLK device,
  supplies the COW/base devices to snapuserd, attaches the device, and in
  first-stage init puts a device-mapper linear layer over the resulting UBLK
  block device. The `dm-user` name remains in some shared helper methods and
  IPC calls, but the UBLK data path is UBLK plus dm-linear; it does not require
  `CONFIG_DM_USER`.
- The generic ramdisk copy is a hard requirement for installed boots: init
  must start the static `snapuserd_ramdisk` binary before mounting `/system`
  through its UBLK snapshot. `snapuserd/Android.bp` lines 171–208 explains the
  early-boot static executable and defines `snapuserd_ramdisk` as ramdisk
  available, with the `snapuserd` symlink. The init rc service definitions are
  [`snapuserd.rc`](../../../../system/fs/fs_mgr/libsnapshot/snapuserd/snapuserd.rc#L1)
  lines 1–17. Our boot artifacts currently comprise `ramdisk.img` and
  `vendor_ramdisk.img` (device `BoardConfig.mk` lines 22–35); conversion must
  ensure the actual ramdisk passed by systemd-boot contains the generic
  `snapuserd_ramdisk`/symlink and the needed init rc. The build comment in
  `vabc_features.mk` lines 16–20 says T+ products put snapuserd in generic
  ramdisk, rather than vendor ramdisk.

### Kernel and boot-control requirements

- UBLK requires `CONFIG_BLK_DEV_UBLK` and its built module, `ublk_drv.ko`, or
  a built-in driver, **available before first-stage snapshot creation**. In
  this checkout, `kernel/base.config:2800` and the merged
  [`prebuilt/kernel.config`](../prebuilt/kernel.config#L2801) both set
  `CONFIG_BLK_DEV_UBLK=m`, and `prebuilt/modules/ublk_drv.ko` exists. Although
  the device kernel fragment says there are no first-stage modules, AOSP
  first-stage init actually loads `/lib/modules/.../modules.load` before
  mounting partitions (`system/core/init/first_stage_init.cpp:190–295,453–476`).
  `modinfo` lists no dependencies for this UBLK module, so install it through
  `BOARD_GENERIC_RAMDISK_KERNEL_MODULES` and its load list, or set the driver
  built-in in a later kernel rebuild. Do not set `CONFIG_DM_USER`; UBLK mode
  avoids it.
- The stable AIDL merge contract and state mapping are described above. The
  project HAL implementation at
  [`native/matonos-bootctrl/BootControl.cpp`](../native/matonos-bootctrl/BootControl.cpp#L348)
  still returns `NONE` unconditionally (348–350) and rejects `SNAPSHOTTED` /
  `MERGING` (455–462). Before enabling VAB, change it to atomically persist
  and return `NONE`, `SNAPSHOTTED`, `MERGING`, and the bootloader's
  `CANCELLED` state in its redundant misc records, preserving across reboot.
  `libsnapshot` writes `SNAPSHOTTED` for unverified snapshots, `MERGING` for
  active or failed merge, and clears to `NONE` for completed/cleared state
  (`snapshot.cpp:3322–3362`). Its snapshot progress/state files in metadata
  remain authoritative for individual snapshot states and merge progress;
  the HAL value is the bootloader-persistent safety marker. While status is
  `MERGING`, bootloader flows must not erase userdata/metadata, and should not
  change active slot per `IBootControl.aidl:128–157`.
- Merge is driven by libsnapshot after the target slot is booted and accepted:
  `snapshot.cpp:1159–1173` requires state `Unverified` and current slot equal
  target; `InitiateMerge` calls snapuserd for each snapshot
  (`snapshot.cpp:1304–1343`). `ProcessUpdateState` polls and acknowledges
  completion (`snapshot.cpp:1435–1485`), while `WriteSnapshotUpdateStatus`
  maintains the HAL merge marker as above. Thus the boot-control HAL must
  correctly report current/active slot and success, in addition to merge
  status, so the slot switch and rollback/merge lifecycle agree.

### Feasibility summary

VABC-over-UBLK is a real end-to-end AOSP architecture: product metadata enables
userspace snapshots; update_engine writes COW operations; libsnapshot allocates
COW between free super extents and `/data`; snapuserd serves UBLK-backed
snapshots; first-stage init launches it before mapping partitions; and
libsnapshot later directs and records merge. **It is not ready in this device
configuration yet:** the kernel driver must be packed into the early ramdisk,
generic ramdisk snapuserd contents need confirmation/configuration, and the MatonOS boot-control HAL
doesn’t yet store VAB merge states. These are device integration gaps, not a
need to port or patch the AOSP snapshot stack.
