# r24 enforcing boot test plan (H1 / M1, 2026-10-04)

Defaults now enforce. No policy fixes, image builds or boots were performed for
H1. This inventory separates a current missing permission from historical
failures and unresolved readiness notes; it is not a claim that everything
listed currently fails. Source: `out/pc-logs/agents/codex-sanity-result.md`
(H1, M1), all owned policy under `sepolicy/` and `systembridge/sepolicy/`,
`NOTES.md`, and the installer/handover diagnostics.

## Known current blocker

- **Linux deployment execution / executable library mapping (M1):**
  `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te`
  grants `matonos_linuxd` only `create_file_perms` on
  `matonos_flatpak_data_file`. That macro does not grant `execute` or
  `execute_no_trans`. Unlike the review's description, the checked-out AOSP
  `system/sepolicy/public/global_macros` does include `map` through
  `rw_file_perms`; executable mappings still require `execute`. `systembridge/sepolicy/system_ext/private/file_contexts`
  labels the entire `/data/matonos/linux` tree with this data type. Flatpak
  deployed ELF binaries and shared libraries therefore lack executable-file access
  through the current policy. This breaks Linux app launches, not necessarily
  Android boot completion. Follow-up must separate immutable executable and
  library deployments from writable application data, rather than broadly
  granting writable-data execution. Test a newly installed app, ELF execution,
  dynamic library loading, CLI and graphical launches, Wayland and Xwayland,
  and a second launch after restarting linuxd.

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
- **memfd labeling:** `NOTES.md` explicitly says to revisit memfd SELinux
  labeling before enforcing. Mainline has no ashmem and this system forces
  `sys.use_memfd=true`. Confirm the compiled policy's memfd-class behavior
  and actual descriptor labels/access across Android app/system domains and
  linuxd/compositor. Existing compositor relay rules name
  `matonos_linuxd_tmpfs:file`; verify they cover the descriptors actually
  used, including read/write/map and FD transfer. Test older SDK apps,
  Wayland shared-memory rendering, AHB/dma-buf rendering and Xwayland.
- **linuxd GPU access:** `HANDOVER-2026-10-03.md` previously says GPU access
  works only permissively. Current bridge policy already grants linuxd
  `graphics_device:dir` reads and `gpu_device:chr_file rw_file_perms`. Keep
  accelerated Linux rendering in the regression matrix; do not list that
  older missing grant as still absent.

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

Cover bridge service discovery/calls, linuxd install/run/remove, compositor
socket and FD delegation, controller access, inputd/uinput, sleepd suspend,
PipeWire/audio selection (HDA/no card/selector failure), Wi-Fi/Bluetooth with
present/missing/hotplugged hardware, and signed add-on mount/module/firmware
paths. These are validation coverage, not additional confirmed missing rules.
Resolve each denial by intended access and precise labels; never copy all
AVCs into blanket allows. The above blockers remain outside H1's fixes.
