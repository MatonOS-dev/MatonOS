# MatonOS shared driver SELinux policy

All MatonOS vendor daemons and the Bluetooth HCI HAL run in `matonos_driver`.
This consolidates the former audio, Wi-Fi, Bluetooth, input, and sleep policy
rules into the fixed files in `sepolicy/matonos/`. The policy grants their
combined hardware, sysfs, input, socket, audio, and property access. This
accepts reduced isolation among MatonOS drivers while keeping Android and the
system bridge on separate domains.

`matonos_driver_exec` is assigned to `/vendor/bin/(hw/)?(matonos|maton)-.*`
and the PipeWire helper binaries. A shared `matonos_socket` labels daemon
control sockets; `matonos_bridge.te` grants the system bridge the only client
access. `vendor.maton.*` and persistent MatonOS properties use one property
type, writable by the driver and readable by the platform components that
consume the state.

## Verification status

- Confirmed there is one `BOARD_VENDOR_SEPOLICY_DIRS` entry, pointing at
  `sepolicy/matonos/`; area vendor policy directories were removed.
- Confirmed the old SELinux domain, socket, and property type names no longer
  occur in policy or area documentation.
- AOSP `checkfc -c` parsed the unified file_contexts regexes successfully.
- Checked that the referenced AOSP macros (`init_daemon_domain`,
  `hal_server_domain`, `unix_socket_connect`, `set_prop`, `get_prop`, and
  `vendor_restricted_prop`) exist in the checked-out policy sources.
- No AOSP build or boot test was run. The coordinator build request is
  recorded in `out/pc-logs/agents/build-requests.txt`.

## Fresh-image test plan

1. Build and boot a fresh image, then run
   `tools/check-selinux-labels.sh` on it. Confirm there are no FAIL results.
2. On the Ryzen 5800X build PC, verify PipeWire, Wi-Fi, Bluetooth, inputd,
   sleepd, and the HCI HAL start in `matonos_driver`; check `ps -AZ` and
   `ls -Z /vendor/bin /dev/socket/matonos`.
3. Verify only `matonos_system_bridge` can use the `/dev/socket/matonos/*`
   endpoints. Exercise the Wi-Fi, Bluetooth, input, and sleep bridge calls.
4. Confirm `system_server`, the audio HAL, and the bridge can read the
   relevant `vendor.maton.*` properties and that the driver can update them.
5. Repeat the boot and missing-hardware checks on Surface Pro 3 and HP
   ProDesk 600 G1. Missing or hotplugged radios and audio devices must not
   prevent boot or change the shared domain assignment.

## H5 / L3 fixed system_ext policy inventory

The preflight file-set guard also covers `sepolicy/system_ext/private`
(`matonos_setup.te`, `matonos_controllers.te`, `property.te`,
`property_contexts`, `file_contexts`) and
`systembridge/sepolicy/system_ext/private` (`matonos_system_bridge.te`,
`file_contexts`, `genfs_contexts`, `seapp_contexts`, `service_contexts`).
H5 changes existing files only. The controller exception is linuxd-owned
uinput with per-session evdev binds: payloads and setup/CLI domains have
explicit uinput/hidraw neverallows; payload evdev ioctl access is enumerated.
No new generic device, sysfs, capability or sandbox uinput permission is
part of H5. Linuxd uses its existing CHOWN capability only on event nodes
resolved from its own UI_GET_SYSNAME; input-device setattr enables ownership
by the verified stub UID without adding an input group to payloads.

## Security zones (2026-10-05)

The enforced model is `[hardware] MatonOS glue daemons/HALs | A Android system | B apps`. `matonos_daemon_zone` holds driver, helper, add-on manager, and audio-selector domains. Android core except the System Bridge cannot call daemon Binder endpoints, find their IChannel services, or connect to daemon Unix sockets. Apps, Flatpak payloads, bwrap, and app-launch domains cannot access daemon Binder endpoints, socket paths, or channel services. Daemons cannot call Android domains except the service manager registration path, cannot call apps or the Bridge, cannot open inet/inet6/packet/netlink-route sockets, and cannot connect to netd/dnsproxy or non-Bridge Android Unix services.

`matonos_hw_reader` provides read-only traversal and reads across sysfs hardware-description types and proc types. EFI variables, ACPI/firmware tables, DMI identity, kernel configuration, writable controls, power/wakeup nodes, debugfs and tracefs are excluded. The sysfs exclusions use dedicated labels or existing dedicated AOSP types; debugfs/tracefs already use AOSP's dedicated filesystem type attributes. Device-node `getattr` is available broadly, while reading nodes is limited to MatonOS hardware-information node types. Writes remain device-specific.

The stock audio HAL previously connected directly to the PipeWire daemon socket. That grant was removed to enforce the Bridge-only boundary. Audio needs a Bridge-mediated data path before it can operate under enforcing policy.

### Read grants removed

- `matonos_driver`: removed broad `sysfs`, `sysfs_net`, and `sysfs_type` grants; replaced with `matonos_hw_reader` and labeled exclusions.
- `matonos_driver`: removed broad `proc` traversal and narrow `proc_asound`, `proc_cmdline`, and `proc_swaps` reads; replaced with `matonos_hw_reader` and explicit exclusions.
- `matonos_audio_select`: removed narrow `proc_asound` reads; it now uses `matonos_hw_reader`.
- `hal_audio_default`: removed its PipeWire data-directory, socket-file, and driver Unix-connect grants to enforce the Bridge-only boundary.

### New neverallows

- Android core except the Bridge: no Binder calls, daemon Unix connects, or channel service-manager find.
- Apps, Flatpak apps, bwrap, and app-launch: no daemon Binder calls, Unix connects, or channel find.
- Daemons: no Binder calls to apps or Android domains other than service manager; no Unix connects outside the daemon zone; no inet, inet6, packet, or route-netlink sockets; no netd/dnsproxy connections.
- Non-daemon domains: no daemon Unix connects or daemon socket-file writes.
- Android core except `matonos_driver`: no access to reserved rfkill/VHCI raw-hardware nodes.

### Verification status

- `git diff --check` passes. No AOSP build or boot was run; the coordinator owns the SELinux policy build.
- No visibility-check script exists in this worktree. `tools/check-selinux-labels.sh` checks image labels and requires a built image.
- A policy compile is required to confirm neverallow compatibility with stock grants.

### Open issue

The PipeWire/audio HAL cross-zone path must be redesigned through the System Bridge, or the zone rule must be revised, before enforcing audio is usable.
