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
