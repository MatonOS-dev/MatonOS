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
