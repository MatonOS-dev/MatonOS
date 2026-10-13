# r24 enforcing boot test plan (H1 / M1, 2026-10-04)

Defaults now enforce. No policy fixes, image builds or boots were performed for
H1. This inventory separates a current missing permission from historical
failures and unresolved readiness notes; it is not a claim that everything
listed currently fails. Source: `out/pc-logs/agents/codex-sanity-result.md`
(H1, M1), all owned policy under `sepolicy/` and `systembridge/sepolicy/`,
`NOTES.md`, and the installer/handover diagnostics.

## Linux v3 validation

The v2 launch-policy diagnosis is retired with that implementation. Use
[the current Linux policy](../docs/LINUX-STUB-SELINUX.md) and
[storage tests](../install/linuxd/STORAGE-LAYOUT.md) for v3. Apps load immutable
code inside their own stub process; linuxd never launches payloads or delegates
runtime sockets/FDs. Policy compilation and a fresh enforcing boot remain
required; no broader execution permission is a fallback.

## M1 blocker already changed on this branch, still to validate

- **Wakeup controls:** M1 reviewed read-only `pc_wakeup`; this branch already
  declares `sysfs_pc_wakeup` and `sysfs_pc_wakeup_info` and grants
  `{ getattr open read write }` on the wakeup-control type in
  `sepolicy/vendor/pc_wakeup.te`. Corresponding `/sys/devices/.../power/wakeup`
  and bus metadata patterns are in `sepolicy/vendor/file_contexts`, restored
  by ueventd. H1 did not add these rules. Verify actual canonical labels and
  successful `enabled` writes for PCI USB controllers, USB root hubs/hubs,
  keyboard/mouse interfaces and serio/i8042; suspend and wake on supported
  real hardware. A stale/unmatched label would still fail. Include coldboot
  and later enumeration; the script only runs once and does not promise
  hotplug wake configuration.

## Known historical enforcing failures / unresolved readiness notes

- **Mesa/minigbm installed boot:** `install/README.md` (2026-09-29 installed
  kernel-options retry) records enforcing denials followed by Mesa/minigbm
  load failure and `gralloc-mapper is missing`. The later permissive success
  did not prove these accesses fixed. Collect exact fresh AVCs for library
  loading, DRM opens and mapper initialization before attributing them to a
  particular missing rule. Check the staged SP-HAL labels against
  `sepolicy/vendor/file_contexts`; test live and installed boots on hardware
  GPUs and software/vgem fallback. Both must reach a visible usable home.
- **Composer allocator discovery and Gatekeeper shared-secret discovery:**
  earlier `install/README.md` diagnostics identify denied service-manager
  `find` operations. Current `sepolicy/matonos/matonos_driver.te` already has
  `hal_client_domain(hal_graphics_composer_default, hal_graphics_allocator)`
  and `hal_client_domain(keystore, hal_gatekeeper)`. Treat these as regression
  tests, not missing grants. Verify first boot with empty userdata, mapper
  availability, keystore/SharedSecret startup, and subsequent boots in A/B.
- **pc_gpu_detect bring-up note:** `sepolicy/vendor/pc_gpu_detect.te:3-5`
  still says the full sysfs/property rules come with enforcing. The file now
  contains sysfs reads, vendor config reads and vendor property setters;
  the fixed driver policy also grants its module-loader transition. The
  comment alone is not proof of a remaining missing allow. Validate PCI and
  DRM directory/link/metadata labels, hardware GPU selection and the vgem
  fallback, module loading, and graphics/Vulkan properties under enforcing.
- **memfd and graphics:** validate v3's app-owned descriptors and rendering
  under the stock app domain. linuxd has no GPU-device grants or compositor
  relay. Cover Wayland and Xwayland without cross-domain runtime FD access.

## Policy scan and complete boot matrix

The owned policy scan found **no `permissive <domain>;` statements and no
TODO-marked missing rules**. The only explicit minimal/permissive bring-up
policy note was `pc_gpu_detect`, discussed above. Treble exception attributes
(`vendor_executes_system_violators`) and `mlstrustedsubject` are active policy
exceptions, not domain-permissive switches. No runtime `setenforce 0`, kernel
`enforcing=0`/`selinux=0` or ignore-neverallow setting was found in owned boot
configuration. These scans cannot enumerate every runtime denial.

For r24, use fresh enforcing live media, the debug live entry, and installed
slot A and slot B on disposable targets. Assert `getenforce` is `Enforcing`
and check `/proc/cmdline`, kernel bootconfig if present, and `ps -AZ`. Test
with blank userdata and a subsequent reboot. Preserve serial/logcat/dmesg
and AVCs with image/source identities and timestamps. Confirm the installer
service starts only on live media, completes a disposable installation, and
BootControl blesses a booted installed slot. No permissive fallback is a pass.

Cover bridge service discovery/calls, linuxd install/update/remove, stub-owned
compositor sockets, inputd/uinput, sleepd suspend,
PipeWire/audio selection (HDA/no card/selector failure), Wi-Fi/Bluetooth with
present/missing/hotplugged hardware, and signed add-on mount/module/firmware
paths. These are validation coverage, not additional confirmed missing rules.
Resolve each denial by intended access and precise labels; never copy all
AVCs into blanket allows. The above blockers remain outside H1's fixes.
