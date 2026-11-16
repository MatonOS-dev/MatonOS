# File-backed system images

## Layout and boot path

Installed disks use GPT partitions `maton_system` (ext4), `metadata`, and
`userdata`. The ext4 filesystem is mounted at `/maton_system`; its `images/`
directory stores the EROFS backing image files. libfiemap's metadata lives at
`/metadata/maton/images`. The full-disk dm-linear wrapper used for ext4 must
have one linear target starting at sector zero. `FiemapWriter` can pop that
single-target mapping to its leaf block device; `GetDevicePathForFile()` now
persists the matching `/dev/block/by-name/<partition>` alias.

First-stage init detects `super` on the boot device. If present, it keeps the
existing super/LP path. If absent, init initializes `maton_system`, mounts it
before logical partition creation, and calls `ImageManager::MapAllImages()`
with the custom metadata and data directories. The metadata partition remains
first-stage mounted first. Image names in the libfiemap LP metadata must be
the final slot names (`system_a`, `system_b`, `vendor_a`, etc.); libfiemap
creates dm-linear devices with those names, which existing `logical` and
`slotselect` fstab entries consume.

The `/maton_system` fstab entry is `nofail` for older super-based installs
that do not have the new GPT partition. In file-backed mode it is required;
missing files or metadata fail the logical partition setup rather than
silently booting an incomplete system.

## Installer and updater call sequence

Create `/metadata/maton/images` and `/maton_system/images` after formatting
and mounting those filesystems. Use libfiemap's direct `ImageManager` API,
which supports explicit directories (the `IImageManager::Open(prefix)` DSU
helper only targets `/metadata/gsi` and `/data/gsi`):

1. `ImageManager::Open("/metadata/maton/images", "/maton_system/images")`.
2. `CreateBackingImage("system_b", image_size, CREATE_IMAGE_READONLY)` to
   reserve the file and record its fiemap extents and LP metadata.
3. `MapImageDevice("system_b", 10s, &device)`, write the complete EROFS image
   bytes to that block device, `fsync()` it, then `UnmapImageDevice()`.
4. Repeat for each partition in the inactive slot. Verify each written image
   before changing the boot slot.
5. Use `GetAllBackingImages()` to enumerate and `DeleteBackingImage(name)` to
   remove an image and its metadata. Keep slot activation and rollback policy
   in the installer/updater, not in libfiemap.

`CREATE_IMAGE_READONLY` marks an image read-only when libfiemap later creates
logical partitions. The mapped image device remains writable during image
population. This sequence is documented for the installer/updater; this
change does not add a separate `maton-imgtool` executable.

## Validation and open issues

The source inspection verified that `CreateBackingImage()` obtains the
backing device through `FiemapWriter::GetBlockDeviceForFile()`, which unwraps
a single linear dm target to its leaf block device, and that `MapAllImages()`
uses the stored partition names to create the logical partitions. Both new
patches pass direct `git apply --check` against their checked-out AOSP
projects. The shared preflight currently blocks module builds because the
patch allowlist has not been updated and several unrelated APK/NDK artifacts
are absent from this worktree. No image build or VM boot was run by this task.

Before release, verify on a freshly installed disk with the full-disk dm
wrapper that creates the ext4 filesystem, and on a legacy super disk:

1. Install to a spare drive. Check GPT names and sizes, the full-disk dm
   wrapper's single linear target starts at sector zero, and ext4 mounts at
   `/maton_system`.
2. Populate `system_a`, `system_ext_a`, `product_a`, `vendor_a`, and `odm_a`
   through the libfiemap sequence above. Confirm `/metadata/maton/images`
   records `maton_system` as the backing partition.
3. Boot slot A freshly. From the serial root shell, inspect init logs for the
   file-backed map path and check `dmsetup ls`, `/proc/mounts`, and the
   mounted EROFS filesystems. Confirm their dm names have `_a` suffixes.
4. Write slot B using the updater sequence, verify image hashes, activate B,
   and cold boot B. Interrupt a separate write before activation and confirm
   A remains bootable. Exercise rollback and ENOSPC handling.
5. Boot a legacy disk that still has `super` but no `maton_system`; confirm
   the standard super LP path succeeds and the optional fstab entry is
   ignored.
6. Repeat boot checks on the Ryzen/RX 6600 build PC, Surface Pro 3, and HP
   ProDesk 600 G1, with other disks attached and with no optional hardware.

The coordinator must perform the image build and fresh-boot verification.
