# Phase D2: linuxd SELinux permissions

`org.matonos.linux` receives a package-specific `matonos_linux_app` domain.
The stock `priv_app` domain would make staging creation and linuxd Binder
access available to every privileged app. The dedicated domain adds no new
privileges beyond ordinary app networking, Flatpak APEX reads/execution,
linuxd Binder, and its staging tree. Package identity is constrained by
`isPrivApp=true` and the exact package name in `seapp_contexts`.

linuxd has no network access, inet group, set-ID capabilities, FOWNER, or
installer-helper transition. Its only Linux capability is CHOWN. It first
changes staged ownership to AID_SYSTEM, then performs mode changes and verity
operations as the file owner. The app's staging root is mode 01733: the app can
create a private UID child without listing other children; the sticky bit
protects other owners' entries.

## Remaining linuxd allow rules

| Rule | Reason |
|---|---|
| `init_daemon_domain(matonos_linuxd)` | Init starts linuxd in its dedicated service domain. |
| `tmpfs_domain(matonos_linuxd)` | Binder/runtime internals need linuxd-private tmpfs labels. |
| `allow matonos_linuxd matonos_linuxd_service:service_manager add` | Register the linuxd AIDL service. |
| `neverallow { domain -matonos_linuxd } matonos_linuxd_service:service_manager add` | Keep service registration exclusive to linuxd. |
| `binder_use(matonos_linuxd)` | Register and receive Binder calls. |
| `allow matonos_linuxd system_file:dir r_dir_perms` | Traverse fixed system helper paths. |
| `allow matonos_linuxd system_file:file { execute execute_no_trans getattr map open read }` | Run fixed Android helper binaries used by Flatpak management. |
| `allow matonos_linuxd matonos_flatpak_cli_exec:file { execute execute_no_trans read open getattr map }` | Execute and inspect Flatpak/OSTree APEX tools. |
| `allow matonos_linuxd self:capability chown` | Take staged files from the app and set the managed owner. |
| `allow matonos_linuxd self:process { fork sigchld signal }` | Spawn and reap fixed Flatpak/OSTree child processes. |
| `allow matonos_linuxd matonos_flatpak_deployment_file:dir create_dir_perms` | Create and manage install/runtime deployment directories and metadata. |
| `allow matonos_linuxd matonos_flatpak_deployment_file:{ file lnk_file } create_file_perms` | Write deployment metadata and links. |
| `allow matonos_linuxd matonos_linux_code_file:dir { create_dir_perms relabelto relabelfrom setattr }` | Publish verified OSTree object directories with their code label. |
| `allow matonos_linuxd matonos_linux_code_file:file { create_file_perms relabelto relabelfrom setattr }` | Publish verified OSTree object files with their code label. |
| `allow matonos_linuxd matonos_flatpak_deployment_file:file link` | Let OSTree link deployment metadata within its private tree. |
| `allow matonos_linuxd self:fifo_file ioctl` | Read the bounded status of its child-process pipes. |
| `allowxperm matonos_linuxd self:fifo_file ioctl { 0x5413 }` | Limit pipe ioctl access to FIONREAD. |
| `allow matonos_linuxd self:process setfscreate` | Assign fixed labels when creating managed storage entries. |
| `allow matonos_linuxd system_data_file:dir { getattr search }` | Reach the init-created encrypted `/data/linux-app` root. |
| `allow matonos_linuxd matonos_flatpak_deployment_file:file ioctl` | Permit OSTree's FICLONE operation on deployment files. |
| `allowxperm matonos_linuxd matonos_flatpak_deployment_file:file ioctl { 0x9409 }` | Restrict deployment ioctl access to FICLONE. |
| `allow matonos_linuxd matonos_flatpak_deployment_file:{ dir file } relabelfrom` | Remove the deployment label when deleting or replacing managed trees. |
| `allow matonos_linuxd matonos_linux_code_file:file link` | Link verified code objects into OSTree deployments. |
| `allow matonos_linuxd matonos_linux_code_file:lnk_file { getattr read relabelto relabelfrom unlink }` | Inspect and reclaim managed code links. |
| `allow matonos_linuxd matonos_flatpak_staging_file:dir { create_dir_perms relabelfrom relabelto }` | Take over, relabel and remove staging directories. |
| `allow matonos_linuxd matonos_flatpak_staging_file:{ file lnk_file } { create_file_perms relabelfrom relabelto }` | Take over, relabel and clean staged files/links. |
| `allow matonos_linuxd matonos_flatpak_staging_file:{ file lnk_file } link` | Import verified staged OSTree objects without copying. |
| `allow matonos_linuxd matonos_flatpak_staging_file:file ioctl` | Apply and measure fs-verity on staged regular files. |
| `allowxperm matonos_linuxd matonos_flatpak_staging_file:file ioctl { FS_IOC_ENABLE_VERITY FS_IOC_MEASURE_VERITY }` | Restrict staging ioctls to the two verity operations. |

The system bridge retains only service lookup and a Binder call for its existing
orphan-deployment reconciliation trigger. There is no linuxd-to-bridge Binder
callback and no bridge access to Linux staging, deployment, or install data.
