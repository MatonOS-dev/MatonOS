# Flatpak management service

linuxd is a late-start system_ext service. Its absence never blocks boot.
The authenticated `ILinuxd` interface serves install/update/store configuration,
metadata, icons, desktop entries, inventories, reconciliation and stub-signer
registration. The System Bridge verifies PackageManager identities; generated
stubs may request only their own installation/update. No runtime-directory,
Downloads, kill or launch-status RPC remains. Rebuild bridge, linuxd and host
with the same AIDL: the interface is private and pre-alpha.

Apps and runtimes use the shared encrypted `/data/linux-app` store. Current
app-id pointers select immutable random version directories; read-only leases
protect running versions and their runtime references. PackageManager owns
per-user stub HOME/cache/runtime data. See [storage](STORAGE-LAYOUT.md),
[ownership](APP-OWNERSHIP-r24.md) and [publication](CODE-STORAGE-r24.md).

`flatpak-env-wrapper` is management-only and rejects `run`. linuxd prepares
fixed, system-owned mode-0700 `/data/linux-app/manager/{home,cache,run,tmp}`
scratch directories. The pinned stub certificate is in
`/data/linux-app/store/stub-signer.der`. No `/data/matonos/linux` tree remains.
The staging helper enters the verified stub UID for network attribution:
SETUID/SETGID remain for that job. CHOWN/FOWNER seal staged objects; SYS_ADMIN
and KILL capabilities and the uinput group are removed. Network policy remains
for remote-add's HTTPS fetch. No app runtime or hardware proxy runs in linuxd.

Kept code: FlatpakManager/Publish/Store (management), RuntimeRefs and
DeploymentStore (lease-aware collection), StubVerify (certificate + live PM
identity), SafePath and FlatpakIcon (confined metadata/icon reads), LinuxStorage
and linux-paths (fixed storage layout), management AIDL and init registration.
Removed v2 launch code, input discovery/pads, Downloads grants, GTK/MLS helpers
and their tests. No new SELinux exceptions were introduced.

Local validation and exact logs are listed in
`out/pc-logs/agents/v2-retire-result.md`; policy/image and enforcing runtime
validation belong to the coordinator. Host compilation is not proof of device
behavior. Fresh-image steps: build policy and all related artifacts together,
run the label checker, boot with fresh userdata, disable idle suspend, install
one X11 and one Wayland app through the store, and verify private runtime
sockets, portals, audio and DNS. Repeat update/lease/multi-user removal checks
from STORAGE-LAYOUT. Confirm another UID cannot open runtime sockets and no
retired path or helper appears. Repeat on Ryzen/RX6600, Surface Pro 3 and
HP ProDesk; no kernel change or hot-patching is required.
