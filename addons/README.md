# MatonOS add-on slots

## Slot and update design

The installer layout reserves two 512 MiB GPT partitions, `addons_a` and
`addons_b` (entries 6 and 7). Each partition stores a complete ext4 image for
the matching OS slot. Init mounts the selected partition read-only at
`/mnt/vendor/addons`, with no `wait` flag. It discovers the block node through
the same `androidboot.boot_devices` and ueventd by-name links used for
`super` and `misc`. A PC without these partitions still boots; the optional
mount and module loads fail closed.

The host packager makes the ext4 image before distribution. The image builder
uses AOSP `mkuserimg_mke2fs`/`e2fsdroid` and the area image contexts to bake in
SELinux labels. The image contains the signed payload manifest, module and
firmware files, `modules.load`, a bounded `modules.dep`, and firmware lookup
links. Modules carry `vendor_addon_module_file` (`vendor_file_type` plus
`contextmount_type`); firmware targets use `vendor_file`. The current service
and host packager reject daemon payloads, so no package-supplied executables
are installed. The user wants packages to support userspace daemons such as
`iptsd`; this is the next implementation step. Keep daemon payloads on the
read-only context-mounted image and execute them through a fixed,
policy-labeled launcher under `/vendor/bin`. The launcher should validate the
signed manifest and an allowlisted command, then use `execute_no_trans` for
the payload. This keeps the only SELinux entrypoint at a recognized vendor
path and avoids runtime writes to executable file types. Add a narrowly scoped
policy rule and manager API for start/stop/status, with bounded startup and a
no-daemon fallback; add daemon package tests before enabling it. All image
paths are read-only at runtime.

The ZIP carries the same module metadata plus an outer signed manifest that
binds the full 512 MiB image SHA-256. The filesystem image carries an inner
signed manifest that binds each module/firmware path, size and SHA-256 without
self-referencing the image hash. The device verifies the repository signature,
ELF metadata and exact running `uname -r` before accepting the image, then
checks the expanded image hash before writing the selected raw partition. The
service unmounts the read-only slot before the block write and requests a
reboot; init remounts the finished image read-only on the next boot. It never
extracts package files onto the mounted slot.

This first installer path targets the currently selected slot and updates one
complete package image at a time. A failed write can make add-ons unavailable
for that slot, but the OS still boots without them. The system updater must
build the image for the exact target kernel and stage it to the inactive
`addons_<slot>` before switching OS slots; that updater integration and
multi-package image composition remain follow-up work. Keeping the previous
slot image provides rollback when OS and add-on slots switch together. Never
force-load a module when release or vermagic differs.

## Package format and signing

`addons/build/make-package.sh` accepts module and firmware inputs and
produces `<package>.zip`, `<package>.zip.img`, and a public-key sidecar. The
image is exactly 512 MiB and is signed into the outer manifest by SHA-256. The
inner manifest in the image includes:

- exact kernel release and module vermagic;
- module ELF name, license, source/provenance and modalias values;
- safe relative module/firmware paths, sizes and SHA-256 hashes;
- image ID and version.

The repository signature uses Monocypher EdDSA/BLAKE2b. The development
public key is [repo.pub](repo.pub) and is installed as
`/odm/etc/addons/repo.pub`; its private key stays outside the tree at
`~/.config/matonos/addons/repo.pem`. Release signing must replace this
development key. Repository signatures authenticate packages; they are
separate from Linux module signatures. `MODULE_SIG_FORCE` remains disabled.
Enabling it needs a kernel configuration change, a protected module-signing
key, and release signing in the kernel pipeline.

Each image is built against the exact kernel source, generated config,
headers and `Module.symvers` for its target release. A different kernel
release requires rebuilding and signing the module and image. Exact vermagic
does not by itself prove ABI compatibility, so candidates still need build and
hardware validation.

## Release repository and OS updates

The add-on repository is versioned with MatonOS, not the Android platform
version. For release `26.12.0`, for example, the static HTTPS directory is
`https://download.hanro50.net.za/matonos/updates/26.12.0/addons/`, beside that
release's OS update payload. The current OS and Settings resolve the running
`ro.matonos.version` and read that release's signed add-on index. Each module
and complete slot image is built for that release's exact kernel. Upload all
payloads before publishing the new index. Index and update-manifest files have
`.hwfm` sidecars with a 60-second cache override, so clients can refresh the
index promptly without sacrificing long caching for immutable payloads.

Before an OS A/B update switches slots, the updater reads installed add-ons,
fetches matching packages and complete slot images from the target release's
`addons/` directory, validates signatures and kernel releases, and stages
them to the inactive add-on slot. It switches the OS and add-on slots together;
the previous pair remains available for rollback. A missing target-release
package leaves that add-on disabled after upgrade instead of carrying a
module with stale vermagic across kernels. The current native service
implements signed local intake and slot writing; release-index download,
`.hwfm` refresh, Settings lookup and updater staging still need integration.

`addons/tools/test-addon.sh` mirrors its generated test ZIP, image and public
key under `out/pc-logs/addons-repo/<ro.matonos.version>/addons/` (or
`dev-<kernel-release>` when the development image has no MatonOS version
property). This mirrors the release server path for review without publishing
anything. Set `MATON_ADDON_TEST_REPO_ROOT` to change the local mirror root, or
`MATONOS_VERSION` to test a specific release directory.

## Runtime loading and Wi-Fi handoff

The service registers the shared `vendor.matonos.channel.IChannel/addons`
channel. The bridge authorizes Settings callers; no area-owned Binder service
or socket is used. The manager validates the signed ZIP and module ELF metadata
against the running kernel, verifies the full image hash, and writes only the
selected add-on raw block device. The image's `modules.load` is read after the
read-only mount; each module is revalidated against the signed inner manifest
before stock `modprobe` loads it. Loads are optional and time-bounded so a bad
module cannot hold boot.

After a supported Wi-Fi module appears, the existing Wi-Fi proxy must observe
its netlink interface event and select it behind Android's stable `wlan0`
interface. That hotplug selection and Broadcom `wl` WEXT compatibility remain
hardware follow-up work. The first VM test uses `mac80211_hwsim` only to prove
the signed image install/load lifecycle; it does not prove the Wi-Fi proxy
handoff.

Firmware files are stored under `/mnt/vendor/addons/firmware/`. The kernel
boot argument fixes direct lookup at `/vendor/firmware`, and this kernel has
`CONFIG_FW_LOADER_USER_HELPER` disabled, so ueventd's extra search directory
does not serve kernel `request_firmware()` calls. Init bind-mounts the OS
firmware tree aside as a private mount and overlays both read-only trees at
`/vendor/firmware`, labeled for kernel firmware reads. The private bind keeps
the new mount from propagating back into its own lower path. If the add-on
mount is absent, the overlay fails harmlessly and OS firmware remains
available. The package builder puts
firmware targets and symlinks in the signed image. Redistributable
`linux-firmware`, SOF and wireless-regdb remain part of the OS image; add-on
firmware is for non-redistributable blobs such as Broadcom Bluetooth `.hcd`
files.

## Status, removal and recovery

`status` and `available` report kernel release, repository-key availability,
mounted-slot state and module validation/load state. A missing or malformed
image is skipped, leaving the OS boot path intact. A mismatched-vermagic
package is rejected before the service unmounts or writes a partition.

The mounted image is immutable. `uninstall` therefore returns an explicit
error; removal and rollback require building and signing a replacement full
slot image, then writing it to the intended slot. Multi-package image
composition, automatic old-version garbage collection and the updater's
inactive-slot handoff are not implemented yet.

## Build and QEMU test

The test fixture in `tests/` is an out-of-tree module that also calls
`request_firmware()` for a dummy file. Once the coordinator reports a fresh
image `OK` and the bridge test hook is bundled, prepare a disposable copy with
the add-on partitions on the boot disk. The live image is single-slot and
ueventd creates `/dev/block/by-name` aliases only for partitions on that
boot disk. The helper adds the two slots and `_a` to both boot entries so
the suffix persists when the VM reboots into the normal live entry:

```sh
addons/tools/prepare-qemu-test-image.sh \
  out/target/product/pc_x86_64/matonos-live-x86_64.img \
  out/pc-logs/addons/matonos-addons-vm-test.img
MATON_VM_SLOTS=1 tools/run-qemu-live.sh \
  -i out/pc-logs/addons/matonos-addons-vm-test.img \
  -g none -m 4096 -a 5556 -s out/pc-logs/addons/serial.log
```

Select **MatonOS Live (debug)** at boot. After `adb connect 127.0.0.1:5556`,
run:

```sh
addons/tools/test-addon.sh 5556
```

The test script stages its signed fixture payloads under the release-keyed
local path described above before submitting them through the debug bridge
channel.

The script builds `matonos_addon_test.ko` against the booted kernel, signs a
matching package and image, submits a signed wrong-vermagic package and checks
that the service rejects it, uploads the image, reboots, checks `lsmod` and
`dmesg`, checks the baked module SELinux label, then reboots a second time to
check persistence. It also checks dummy firmware delivery. Use one headless
VM at a time, no more than 4096 MiB, never port 5555, and stop the VM after
collecting evidence. Run `tools/check-selinux-labels.sh` on the fresh image.

On 2026-09-28, a fresh image built at 13:24 booted with kernel
`7.2.7-dirty`. The coordinator's LineageOS-style bridge signing fix restored
PackageManager registration for `org.matonos.systembridge`; the `addons`
status call succeeded. The harness built and signed an out-of-tree test module
plus dummy firmware, rejected a signed package with mismatched ELF vermagic,
and accepted the valid package. The image was written to `addons_a`; after
reboot the module loaded and its ELF file had the `vendor_addon_module_file`
label. A second reboot also reloaded it. The first commit attempt exposed a
1 MiB raw-copy buffer on a bounded channel callback stack; reducing it to
64 KiB fixed the crash, and the next fresh-image run committed successfully.
The firmware file was installed but initially not served because the kernel's
direct lookup is pinned to `/vendor/firmware` and no firmware usermode helper
is configured. A read-only overlay of the OS and add-on firmware trees has
now been added to init and fixed SELinux policy. Fresh-image verification of
firmware delivery worked on the 13:52 image in permissive mode: the test module
received its 29-byte blob and the OS `regulatory.db` remained visible. That
run also showed the overlay mount propagating to the bind-mounted base path.
The later 15:05 image still showed this propagation despite the init `private`
command. See `SHARED-CHANGES.md` for test evidence and remaining work.

The fresh full image built at 15:05 on 2026-09-28 was tested in headless QEMU
on port 5557 using a second virtual disk with `addons_a` and `addons_b`. It
booted with kernel `7.2.7-dirty` in permissive mode. The harness rejected the
signed mismatched-vermagic package, accepted the valid package, wrote the
signed image to the raw slot, and rebooted. `lsmod` showed
`matonos_addon_test`; `ls -Z` showed `vendor_addon_module_file`; `dmesg`
reported the module loaded and its 29-byte test firmware served. A second
reboot again loaded the module. The firmware overlay showed the expected
`matonos_addon_firmware_overlay` context and retained the OS
`regulatory.db`.

The image's mount table also exposed an unresolved propagation issue: the
`/vendor/firmware` overlay appeared over `/mnt/vendor/firmware-base` too, and
that mount remained in shared group 10 despite the init `private` command.
Current functionality works in this QEMU run, but the base-mount isolation
needs a corrected init mount sequence and a fresh build/test. All runtime
verification so far is permissive; enforcing-mode behavior remains unverified.

## Real PC test plan

On the Ryzen 5800X/RX 6600/Intel 7265 PC, install an image built for its exact
kernel, verify the proxy presents stable `wlan0`, then test OS/add-on slot
update and rollback. On Surface Pro 3, verify the in-tree Marvell 88W8897
Wi-Fi/Bluetooth and normal touch continue working without an add-on. On HP
ProDesk 600 G1, boot with no add-on partitions and verify in-tree Intel HDA.
Also test bad signatures, mismatched releases, interrupted image uploads,
missing partitions and failed modules; none may prevent boot.

The first real `wl` candidate needs a redistributable build compatible with
kernel 7.2 and a working WEXT backend. Surface IPTS needs a userspace `iptsd`
port and hardware test. Neither is enabled by this test image.

## Shared integration

See [SHARED-CHANGES.md](SHARED-CHANGES.md). The coordinator added the service,
init rc, repository public key, `addons` VINTF channel, Settings authorization
row, and root-only live-userdebug bridge test hook in the 2026-09-28 06:18
image. No AOSP patches or new SELinux policy files are used.
