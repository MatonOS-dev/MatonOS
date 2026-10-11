# Linux stub SELinux (2026-10-11)

Generated `flatpak.*` stubs belong to the Android security zone and run in
stock `untrusted_app_*`, selected by targetSdk, with stock `levelFrom=all`.
They have no custom domain and no `mlstrustedsubject`. A device-generated key
cannot be represented by a build-time seinfo mapping; package names are not
identity. linuxd and the System Bridge verify the device signing certificate.
Direct stub↔linuxd Binder is the scoped exception to the bridge rule. The
bridge separately authenticates and scopes stub window calls. SELinux grants
the coarse transport to `untrusted_app_all`, including ordinary Android apps;
it cannot distinguish signed stubs within that attribute.

`matonos_linuxd_service` is `app_api_service`, not `protected_service`.
Only linuxd may register it. No isolated/ephemeral app transport is added.

## Runtime sockets

Paths stay `/data/matonos/linux/tmp/<uid>/{wayland-0,bus,...}`. This is the
existing device-wide runtime root, not the DE Flatpak deployment directory
`/data/misc_de/<u>/matonos-linux/runtime/<runtime_uid>`. linuxd authenticates
`prepareRuntime`, derives the UID categories, pre-creates the root as UID:UID
0700 using thread-local fscreate, and checks the pinned inode's owner and exact
label. Existing mismatched roots fail closed; no migration is attempted.
The in-process compositor changes it to 0711 so the system-UID supervisor can
traverse to its control endpoint. Only the owner can list/write the directory.

Use stock `media_rw_data_file` for this internal-storage runtime tree. Stock
untrusted apps already create directories and ordinary files with this type;
the added permission is socket-file creation. Newly created sockets carry the
stub's categories. linuxd's existing `mlstrustedsubject` permits connection;
`matonos_linux_app` carries the same verified UID categories as the stub, so
needs no MLS trust. bwrap binds the runtime directory using `mounton`, with
existing read/search rights. It receives no new stub socket connection right.
All stock `untrusted_app_*` variants are already coredomain in this checkout,
so the connections are system-side under Treble.

This is a coarse shared type, also used by Downloads. It is NOT a per-app
file-type/MLS isolation boundary: `media_rw_data_file` is `mlstrustedobject`.
UID ownership, directory modes, socket peer authentication, and the per-app
mount namespace provide scope. The payload sees its own runtime tree only;
existing Downloads grants remain separate mount capabilities. Added socket
permissions apply to this type everywhere, so DAC and endpoint checks matter.
No Android app is granted Linux-zone home/code data access.

Alternatives reviewed: a custom `app_data_file_type` is outside the literal
create/unlink whitelist; `app_data_file` would prohibit linuxd directory
creation/relabel, non-app directory traversal and bwrap `mounton`. Assigning
MLS trust alone cannot override those neverallows. No stock neverallow is
modified and no exception attribute is added.

Stock GPU access comes from `private/technical_debt.cil:71` assigning
non-isolated appdomain members to `gpu_sphal_domain`, and
`private/gpu_sphal_domain.te:3-4`. Custom stub GPU/graphics grants are removed.
The `graphics_device` character-device neverallow remains intact.

## Validation

Text review only. See the [agent report](/mnt/data/aosp/out/pc-logs/agents/stub-untrusted-result.md)
for the stock neverallows checked and pending policy/device validation.
Dynamic runtime labels must not be recursively restorecon'd to the general
`matonos_flatpak_data_file:s0` file-context default. Such a reset fails the
runtime-root validation; label repair is not included in this change.
