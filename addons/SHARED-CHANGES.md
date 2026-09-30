# Add-ons shared changes

Completed in the fixed shared SELinux policy:

- `sepolicy/matonos/matonos_driver.te` declares the `addons_a/b` block type,
  the manager domain, and the image module type.
  The manager writes only `/dev/block/by-name/addons_<slot>` plus its private
  `/data/vendor/matonos-addons` staging files. It does not write files on the
  mounted add-on filesystem. Package daemon payloads are rejected in this
  release. `vendor_modprobe` can load only `vendor_addon_module_file` modules.
- `sepolicy/matonos/file_contexts` labels raw GPT entries 6/7 and their
  by-name aliases, add-on mount paths, image module/firmware/payload files, and
  the staging directory. The same image labels are supplied to the host
  `e2fsdroid` builder through `addons/image_file_contexts`.
- `sepolicy/matonos/matonos_driver.te` also grants init the mountpoint
  directory creation it needs and labels the read-only firmware overlay for
  kernel reads. This follows the fixed-policy-file requirement.
- `sepolicy/matonos/service_contexts` registers the stable `IChannel/addons`
  instance.
- `ueventd.rc` has a marked `# addons:` firmware search directory at
  `/mnt/vendor/addons/firmware/`, after `/vendor/firmware/`.
- `device.mk` has a marked `# addons:` include for `addons/addons.mk`.

Area-owned init mounts `/dev/block/by-name/addons${ro.boot.slot_suffix}`
read-only without a `wait` flag. A missing partition therefore does not delay
boot. The service verifies the signed package and module metadata, then checks
the repository-signed image hash before writing the raw slot block device.
The installer layout supplies writable 512 MiB ext4 partitions at GPT entries
6 and 7. Discovery follows `androidboot.boot_devices` and ueventd by-name links.

Coordinator integration (applied for the 2026-09-28 06:18 image):

- Add `buildinfra/native-built/matonos-addons-service`,
  `addons/matonos-addons-service.rc`, and `addons/repo.pub` mapped to
  `/odm/etc/addons/repo.pub` in `bundle/contents.list` under `# addons:`.
- Add the `addons` instance to `bundle/matonos-channel.xml`.
- Add the `org.matonos.settings addons` authorization row to
  `systembridge/res/raw/target_caller_allowlist.txt` for the future Settings
  UI.
- For tonight's integration script, add the debug-only `addons_test_call`
  method matching `installer_test_call`, gated by root UID,
  `Build.IS_DEBUGGABLE`, `ro.boot.matonos.live=1`, and
  `persist.vendor.maton.addons_test=1`. It must forward only `status`,
  `available`, `begin_package`, `package_chunk`, `commit_package`,
  `begin_image`, `commit_image`, and `uninstall` to the `addons` channel.

The coordinator reports that the bridge signing collision is fixed. On the
fresh 12:41 image, PackageManager registered SystemBridge and
`addons_test_call` worked after enabling the live debug hook as root. In
port-5557 headless QEMU, an out-of-tree test module built against
`7.2.7-dirty`; the correctly signed package passed validation and the
deliberately altered-vermagic package was rejected. `commit_image` first
exposed a 1 MiB stack buffer in the bounded channel callback's raw block write
path. Changing it to 64 KiB fixed that crash. On the 13:24 fresh image,
`commit_image` succeeded. After reboot, `lsmod` showed the module and `ls -Z`
showed `vendor_addon_module_file`; a second reboot reloaded it.

The dummy firmware file and lookup symlink were present, but `request_firmware`
reported `Direct firmware load ... failed with error -2`. Kernel config has
`CONFIG_FW_LOADER_USER_HELPER` unset, and the boot argument fixes direct lookup
to `/vendor/firmware`; therefore the new ueventd search path cannot serve this
kernel's request. Add-on init now binds the original OS firmware directory
aside and mounts a lower-only read-only overlay at `/vendor/firmware`, with
both OS and add-on firmware trees and a dedicated context-mount type readable
by the kernel. On the 13:52 image, the populated-slot QEMU run showed that the
overlay worked in the permissive image: `request_firmware()` received the
29-byte add-on blob and the OS `regulatory.db` remained visible. `lsmod` showed
the test module, `ls -Z` showed `vendor_addon_module_file`, and a manual second
reboot reloaded the module. SELinux audit logged a denied `mounton` for
`/mnt/vendor/firmware-base` on that permissive image. The rule is present in
the fixed policy and compiled in the 15:05 image; runtime enforcement still
needs an enforcing-image test.

The fresh full image built at 15:05 on 2026-09-28 was tested in headless QEMU
on port 5557 using a disposable disk with `addons_a`/`addons_b`. The VM used
kernel `7.2.7-dirty` and booted in permissive mode. The test harness rejected
the signed mismatched-vermagic package, accepted the matching package and
firmware, wrote the image to the raw block slot, and rebooted. `lsmod` showed
`matonos_addon_test`, `ls -Z` showed `vendor_addon_module_file`, and `dmesg`
reported the module loaded and firmware served (29 bytes). A second reboot
reloaded the module. The firmware path had
`matonos_addon_firmware_overlay`; the OS `regulatory.db` remained visible.

The attempted init `private` operation did not isolate the bind mount: mount
info still showed `/mnt/vendor/firmware-base` in shared group 10 and the
overlay mounted there as well as at `/vendor/firmware`. Functionality passed,
but the mount sequence needs correction and a fresh build/test. Runtime
verification is permissive only; enforcing mode remains unverified. The
15:05 full build passed policy compilation and the image's
`check-selinux-labels.sh` passed. `build-native.sh` and `preflight.sh` also
pass. Earlier failed builds due the unrelated `frameworks/base` patch and the
interrupted VM run have been superseded by this successful run. The harness
mirrors generated payloads under the release-keyed local path and truncates
adb errors to avoid logging base64 arguments.

`addons/repo.pub` is the raw 32-byte development Monocypher key. The matching
private key is only on the build host at `~/.config/matonos/addons/repo.pem`;
release packaging must replace the development pair. Package-provided daemon
launch is rejected in this immutable-slot release. Uninstall and
rollback require a newly signed replacement image and are documented in the
area README.

Release repository decision from the coordinator (2026-09-28 14:00): the
versioned tree lives at
`https://download.hanro50.net.za/matonos/updates/<matonos-version>/addons/`,
beside the OS payload. The running release's service/Settings use its signed
index; the updater fetches the target release's add-ons before A/B slot
switch. Index `.hwfm` sidecars carry the 60-second cache override; payloads
must be uploaded before publishing the index. README now records this and the
test harness mirrors fixture files below a release-keyed local path. Index
fetch, `.hwfm` refresh, and updater integration remain future implementation
work; no shared files were edited for them.

No AOSP patches or new SELinux policy files were added. Shared files were
integrated by the coordinator as listed above; this area made no new shared
file edits during final verification. The user-space daemon payload launcher
remains deferred: next step is a fixed `/vendor/bin` launcher with
`execute_no_trans` for payloads on the context-mounted image.
