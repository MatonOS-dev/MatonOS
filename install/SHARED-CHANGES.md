# Install area shared integration

The overnight goal authorizes these shared additions. The UI caller is MatonOS
Settings (`org.matonos.settings`), per the 2026-09-27 user note; no second
Expo installer app is imported.

## Applied

- `bundle/contents.list`: added the NDK service binary and its ODM init rc.
- `bundle/matonos-channel.xml`: registered the optional `install` instance.
- `systembridge/res/raw/target_caller_allowlist.txt`: added the exact
  `install org.matonos.settings` caller row. Settings already has a pinned
  signing certificate and the ordinary `SYSTEM_BRIDGE` permission.
- `sepolicy/matonos/matonos_driver.te`: added installer-specific raw block
  write permission inside the required fixed `matonos_driver` domain. Installer
  block nodes use a dedicated SELinux type; fixed system commands run through
  the existing helper execution path.
- `sepolicy/matonos/matonos_driver.te`: allow the stock DRM composer to create
  its kernel uevent listener socket; generic virtio-vga otherwise prevents
  SurfaceFlinger from starting on the installed target.
- `sepolicy/matonos/service_contexts`: label the software Gatekeeper's
  `ISharedSecret/gatekeeper` companion interface as `hal_gatekeeper_service`,
  the service type its existing HAL domain may register. The AOSP shared-secret
  type is reserved to KeyMint by a platform `neverallow`, so no permission
  override or APEX removal is used.
- `sepolicy/matonos/file_contexts` and `service_contexts`: labeled the
  installer binary and channel instance, plus common NVMe, MMC, SATA/SCSI,
  and virtio block nodes.
- `sepolicy/matonos/matonos_bridge.te`: the existing system-bridge to
  `matonos_driver` Binder permission serves the installer channel.
- `sepolicy/matonos/matonos_driver.te`: added read-only access to proc mount,
  swap, and command-line state; the service refuses disk writes if any safety
  snapshot is unavailable, and the live-only gate uses the kernel command line.
- `systembridge/SystemBridgeService.java`: added a root-only end-to-end test
  forwarding method. It is available only on a debuggable live image after
  root sets `persist.vendor.maton.installer_test=1`; production callers still
  go through the normal package, certificate, target, and permission checks.
- `native/matonos-installer-service`: added the NDK target. The fixed
  `tools/build-native.sh` registry discovers projects under `native/`.
- `native/matonos-bootctrl`: added an NDK-built stable AIDL boot-control HAL,
  VINTF registration, and init service. It stores slot state in misc and finds
  the ESP/misc partitions on the same physical disk as the active system slot.
  The AOSP AIDL default HAL and generic device manifests were checked; no
  generic x86 systemd-boot integration exists, and the default relies on
  Android bootloader-message control, so it cannot switch these EFI entries.
- `BoardConfig.mk` and `fstab.pc_x86_64`: enabled A/B dynamic partitions and
  slot-select fstab entries. `fstab.pc_x86_64.live` keeps its live suffix and
  selects the one A-only logical set.
- `tools/make-live.sh`: packages the A-only live super and signed live/debug
  plus installed A/B UKIs; the live ESP is now 1 GiB to fit all four UKIs.
- `rn-apps/settings/src/installer/createV1Plan.ts`: the per-slot entries include
  `androidboot.hardware=pc_x86_64`, the installed fstab suffix, the matching
  `_a`/`_b` slot suffix, `androidboot.boot_part_uuid` from the ESP PARTUUID,
  and `androidboot.boot_devices` derived from the selected disk identity, so
  first-stage fstab lookup and GPT by-name links can resolve on generic buses.
  QEMU exposed that the first target-only entry
  omitted the explicit hardware value; the UKI has it embedded, but the loader
  option now states it as well.
- `bundle/contents.list`, `sepolicy/matonos/service_contexts`, and the fixed
  `matonos_driver.te`: register/label the boot-control HAL in the required
  existing policy file set. Settings uses the live property gate for its
  installer alias; init starts the service only for live mode.
- **Superseded, removed 2026-09-29:** an earlier version mounted a `userstage`
  partition and added a first-boot file importer plus `copy_user_files` API.
  The user decided to skip migration; none of these components remain.
- `device.mk` already includes `install/install.mk` and `BoardConfig.mk`
  already includes `install/BoardConfig.mk`; no include edit was needed.

## Fresh image and target boot follow-up (2026-09-29)

A coordinated full image completed at 04:14 and its Treble label check passed;
a newer full image including the corrections completed at 05:59, also with no
Treble label failures. The target-only retry exposed two boot details: first-stage init needs the
target ESP PARTUUID in `androidboot.boot_part_uuid`, and Android metadata must
be formatted ext4. The Settings planner and `install/tools/test-install.sh`
now include those values/format operations. The user migration path remains
removed. The corrected script was run against a blank 64 GiB target using the
05:59 image. GPT, filesystems, A/B LP metadata, and EFI payload write passed;
the coordinator then stopped the VM for a queued GLib module build during
loader configuration write. No partition copies or installed boot were
completed. Two later VM attempts were interrupted by coordinator-started
`libgmodule-2.0_matonos` and `libgobject-2.0_matonos` builds. The first stopped
before ADB came online; the second reached live mode with the installer
service running, then the coordinator stopped it before installation began.
These module builds followed repeated failures in GLib sources owned by the
separate Flatpak spike. The latest status is RUNNING on the gobject module.
The installer image is already current; no further installer image build is
needed. Rerun when the shared build queue stops.

Shared A/B integration: BoardConfig.mk sets AB_OTA_UPDATER and super group
sizes; the installed and live fstabs use slotselect; tools/make-live.sh creates
an A-only super and four signed UKIs. These changes are deliberately shared
boot/build inputs, with the live fstab and cmdline preserving live boot.

## Flatpak system service integration (2026-09-28)

- `install/install.mk` packages `matonos-linuxd`; `install/linuxd/Android.bp`
  builds the system_ext native service and installs its init rc. The
  boot-completed trigger and `disabled` service declaration keep it off the
  boot-critical path.
- A small local system AIDL (`ILinuxd`) connects System Bridge directly to the
  service; it does not register a vendor channel or VINTF instance. Its
  matching AIDL sources live in the native service and the bridge, which need
  separate Soong roots.
- `systembridge/sepolicy/system_ext/{public,private}` declares the
  system_ext `matonos_linuxd` coredomain, private service-manager label, and
  dedicated `matonos_flatpak_data_file` type. Existing fixed file-context
  rules label `/system_ext/bin/matonos-linuxd` and `/data/matonos/linux`.
- `systembridge/SystemBridgeService.java` adds the `flatpak` target and the
  root-only, live-debug `flatpak_test_call` hook; the target caller allowlist
  authorizes only pinned built-in `org.matonos.settings`.
- No AOSP source patch, D-Bus exposure, kernel change, or host-command
  spawning is added. The service invokes only the fixed Flatpak CLI. The
  system-side domain is required because SELinux rejected labeling a
  system_ext executable and `/data` tree as vendor `matonos_driver` types.
- Runtime integration remains open: upstream Flatpak defaults to its
  system-helper, which uses the system bus for non-root system installs.
  `matonos-linuxd` currently runs as `system`; confirm the spike build flags
  and install behavior before enabling D-Bus. If the helper path is required,
  record the minimal bus/helper design as a shared change first.

## 2026-09-28 build follow-up

- `install/linuxd/Android.bp`: wrapped the local AIDL declarations in a
  filegroup rooted at `aidl`, fixing Soong's generated-output path mismatch.
  The generated platform C++ backend uses `org::...` and `String16`; the
  service source has been aligned to that generated interface. A later full
  build passed the installer SELinux policy checks but stopped in
  `matonos-linuxd` before producing an image. Preflight now passes after the
  coordinator formatted glib's Android.bp. A current full image and target-only
  verification are pending.

## User-file migration removal (2026-09-29)

Per the user's repeated decision, the implementation no longer migrates user
files. Removed `userstage` from the installed GPT and fstab, removed the
`copy_user_files` bridge operation and Settings planner shape, deleted importer
logic and its init trigger, and removed FUSE/shared-storage SELinux rules from
the fixed `matonos_driver.te`. Installed `/data` is initialized empty with new
FBE state. These are DONE edits in owned/shared integration files; no separate
first-boot service or staging resource remains.

## Installed QEMU boot follow-up (2026-09-29)

- `sepolicy/matonos/matonos_driver.te`: added the stock composer uevent socket
  permission after the target-only virtio-vga boot exposed its denial.
- `native/matonos-bootctrl/BootControl.cpp`: slot-state reads now log disk
  discovery failures and return current-slot defaults when `misc` is not
  available yet, so early framework services can continue booting. Slot-state
  writes still require the discovered `misc` partition.
- Both changes need a new coordinated full image before target-only validation.
- Target-only boot confirmed `_a` and the boot-control daemon was running.
  Graphics uevent denials were absent, but Gatekeeper aborted while registering
  `ISharedSecret/gatekeeper`. The coordinator directed us to preserve this
  security HAL and use its authorized service type. The matching context is now
  added; rebuild and verify before claiming installed boot.

## Latest coordinated full-build blocker (2026-09-29)

The C++ source compile for `matonos-linuxd` now passes, but its link fails on
`android::RefBase`, `String8`, `String16`, `strzcmp16`, and `flatpak_manager_*`
undefined symbols. This Flatpak service is outside install ownership. See the
exact symbol diagnostics in `/mnt/data/aosp/out/pc-logs/test-build.log` and the
reported follow-up in `build-status.txt`. Installer core, aggregate NDK daemons,
Settings APK staging, and preflight have passed; no new image exists yet.

## 2026-09-29 allocator and Gatekeeper client policy

**DONE in the existing fixed policy file; full image verification pending.**
Target boot diagnostics showed drm_hwcomposer was denied access to its declared
AIDL minigbm allocator, and keystore was denied access to the Gatekeeper
`ISharedSecret/gatekeeper` instance. Added the matching HAL client relationships
to `sepolicy/matonos/matonos_driver.te`. The service context for the Gatekeeper
companion remains `hal_gatekeeper_service`, so Gatekeeper can register it
without violating AOSP's `hal_sharedsecret_service` registration neverallow.
No new SELinux file or AOSP patch was added. Preflight passes. A new image and
fresh live/install/target-only QEMU run are pending. No other shared-file
change is requested.

## Target-only graphics mapper service context (2026-09-29)

**DONE in the existing fixed policy file; full image verification pending.**
The 14:05 image's target-only boot log showed drm_hwcomposer denied `find` on
`mapper/minigbm`, which had fallen back to `default_android_service`. Added
`mapper/minigbm` -> `hal_graphics_mapper_service` to
`sepolicy/matonos/service_contexts`. This gives the already-declared graphics
allocator client relationship the expected mapper service type; it adds no
policy file and makes no AOSP patch. The fresh live bridge install completed
all 21 requests before this target-only diagnosis. Request a full image build
and repeat live plus target-only verification.

## Installed boot command line and deferred SELinux policy (2026-09-29)

The installed-boot blocker was incomplete kernel options in the generated
systemd-boot entries. The installed kernel therefore ran SELinux enforcing;
denials for `/vendor/lib64/libdrm.so` and `vendor_maton_mesa_prop` prevented
Mesa/minigbm from loading, and libui then reported a missing gralloc mapper.
This is not a VINTF blocker. The coordinator added
`androidboot.matonos.live=0`, `androidboot.selinux=permissive`,
`androidboot.verifiedbootstate=orange`, `firmware_class.path=/vendor/firmware`,
`console=tty0`, and the remaining live console/display options to both A/B
entries in the host test script and Settings plan. The 15:26 image can be
retested without rebuilding it.

Deferred enforcing-mode SELinux policy gaps:
- SurfaceFlinger/system graphics clients need the correct `same_process_hal_file`
  access for `/vendor/lib64/libdrm.so`.
- The zygote property client needs read access to `vendor_maton_mesa_prop`.
- `matonos_driver` also logged block-device access denials while the boot
  control service inspected target `misc`/ESP; validate and add only the
  required fixed-policy rules when returning to enforcing mode.

These policy items belong in the existing fixed policy files. No new policy
file or AOSP patch was made for this test.

## Fresh target retry: ESP filesystem defects (2026-09-29)

The coordinator's corrected EFI options were present in both generated slot
entries. The fresh 18:14 image booted live to `sys.boot_completed=1`, and a
new blank 64 GiB NVMe completed all 21 bridge operations. On target-only boot,
OVMF reported that it could not rename `A+3-0.conf` to `A+2-1.conf` and Android
did not expose ADB within four minutes. This supersedes the earlier
checkpoint/Vold follow-up as the first observed failure on the current
unmodified entry; it is not a VINTF finding.

Offline `fsck.vfat -vn` inspection of the target ESP found colliding 8.3
aliases for `matonos-a.efi` and `matonos-b.efi` (`MATONO~1.EFI`) and `..`
directory entries that pointed at the FAT32 root cluster instead of zero.
`install/service/FatFilesystem.cpp` now emits unique aliases and a correct
root-parent entry. `build-native.sh` and `preflight.sh` pass; a coordinated
build and new install/target-only boot are pending. No AOSP patch or shared
SELinux edit is part of this fix.

The 19:49 coordinated full build compiled and staged the installer bundle but
later failed on the out-of-area `MatonFossifyClock` uses-library check. Its
retry failed at out-of-area `MatonFlathub` `npm ci` before native staging. The
requested bundle-only repack also entered the full build path and failed at
that npm step. The current image remains the 18:14 image; the corrected ESP
writer has not yet been boot-tested.

## Plain Virtual A/B over UBLK implementation (2026-09-30)

Install-owned product fragments set the VABC compression method to `none`,
enable `snapuserd_ramdisk` and UBLK, and request an ODM image so the
installer's `odm_a` source exists. The LP writer accepts zero-extent `_b`
placeholders and marks metadata as a Virtual A/B device; the validator budgets
allocated extents rather than both future slot capacities. Settings and the
host test plan omit `_a` to `_b` clone operations. The boot-control HAL now
persists AOSP snapshot merge status in its redundant misc records and starts
with only slot A bootable.

No shared integration file was edited: install product and BoardConfig
fragments are already included by the device tree. The Settings planner change
is in `rn-apps/settings/src/installer/createV1Plan.ts`, outside this area, and
produces the bridge operation plan. A fresh full-image build and target-only
install verification remain pending.

The 11:57 retry passed product configuration with `TARGET_COPY_OUT_ODM :=
odm`, then failed during Soong generation in the Flatpak-owned generated
filesystem module (`Path is outside directory: ../libexec`). This is outside
the install area; no Flatpak files were changed. Retry the install image build
after the owning area fixes that generated prebuilt path.

After that path was fixed, the 12:06 full build completed at 12:37. BT-fix,
Flatpak, and install all independently saw the live image reboot to EFI before
ADB. The install debug UKI isolated the cause: live `fstab` requires EROFS for
`/odm`, but the install-owned BoardConfig had created `odm.img` as ext4. Changed
`BOARD_ODMIMAGE_FILE_SYSTEM_TYPE` to `$(TARGET_RO_FILE_SYSTEM_TYPE)` (EROFS).
Request another full build, verify live boot first, then continue install.

The 13:15 full rebuild contains EROFS `odm.img` (`file` confirms the EROFS
filesystem). BT-fix's fresh boot on 5567 ended at the EFI countdown after six
minutes with ADB offline. The coordinator's 5570 boot also stalled at the EFI
countdown. Later, after that VM was no longer listed, I booted a home-disk
copy on 5565. The normal entry remained ADB-offline for five minutes after the
UEFI menu; the debug entry reached Android init but SurfaceFlinger aborted
repeatedly. The tombstone says `couldn't find an OpenGL ES implementation,
make sure one of persist.graphics.egl, ro.hardware.egl and ro.board.platform
is set`. The debug shell confirmed `ro.hardware.egl` and
`ro.hardware.gralloc` are unset while `/vendor/lib64/egl/libEGL_mesa.so`
exists. The generated `/odm/etc/build.prop` in
`out/target/product/pc_x86_64` contains only AOSP defaults and lacks the Mesa
selectors registered in `bundle/contents.list`.

This is an ODM image staging collision introduced by enabling
`PRODUCT_BUILD_ODM_IMAGE`: `tools/build.sh` first writes the driver bundle to
`$PRODUCT_OUT/odm.img`, then the AOSP build generates its own image at that
same path. The bundle properties and payload are therefore not the image
included by `make-live.sh`. The evidence points to this build staging issue,
not a VINTF blocker.

**Shared build integration needed:** in `tools/build.sh`, stage the bundle at a
distinct path such as `$PRODUCT_OUT/odm-bundle.img` (`build-bundle.sh -o ...`);
in `install/BoardConfig.mk`, set `BOARD_PREBUILT_ODMIMAGE` to that staged file;
in `install/install.mk` and `install/BoardConfig.mk`, remove
`PRODUCT_BUILD_ODM_IMAGE := true` and `BOARD_ODMIMAGE_FILE_SYSTEM_TYPE` so
AOSP uses the prebuilt rather than generating a competing image. Keep
`TARGET_COPY_OUT_ODM := odm`. The owning build integration agent should make
these changes and request one full image build. Verify the installed
`odm.img` is EROFS and contains the Mesa selectors before repeating the live
boot and install test. I have not edited `tools/*` or graphics-owned files.
