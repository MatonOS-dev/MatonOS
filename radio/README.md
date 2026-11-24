# Radio vendor APEX

`com.matonos.hardware.radio` groups the updateable radio executables behind one
vendor APEX. Its payload is enumerated in `apex/payload.list` and contains:

- the MatonOS fork of `android.hardware.bluetooth-service.matonos`;
- the MatonOS fork of the AIDL `android.hardware.wifi-service.matonos`;
- stock `wpa_supplicant` and `hostapd`;
- `matonos-wifid` and `libmatonos-ipc.so`;
- the service rc and VINTF fragment.

The payload key is `$MATON_RELEASE_KEY_DIR/com.matonos.hardware.radio.pem`; its
version counter is the sibling `.version` file. The script creates the payload
key on first run, with a restrictive umask. Both signing files remain outside
the repository. The APEX is built after the first Soong pass by
`tools/build-radio-apex.sh`, signed with the common release container key, and
selected as a `prebuilt_apex` in the final pass. `tools/lib/vendor-apex.sh` and
`vendor-apex.py` contain shared vendor-library closure, `requireNativeLibs`,
filesystem config, apexer/signapk and compatibility-record generation. Audio
uses the same helpers.

## Init and missing-hardware behavior

The radio rc is imported from the activated vendor APEX. Its
`apex.all.ready=true` action uses `start` for the Wi-Fi and Bluetooth HALs and wifid; it
contains no blocking `exec`, `exec_start`, or `wait_for_prop`. The AOSP
`system/core/rootdir/init.rc` runs `apexd-bootstrap` early, starts apexd after
`/data` is mounted, and waits for `apexd.status=activated` before continuing.
The stock hostapd rc also uses `apex.all.ready`, so this is the established
radio service trigger after the APEX set is mounted. Wi-Fi's lazy AIDL
services are declared in the APEX and start only when requested. The HAL,
supplicant, and hostapd services are not marked `critical`.

`wifi/matonos-wifid.rc` remains in the ODM bundle for the existing post-fs-data
Wi-Fi data directories, cfg80211 modprobe, and hwsim fallback. Its old service
entry and startup are removed; the APEX owns the wifid service. Startup makes
one snapshot and at most one persisted-interface rename attempt. Name
allocation is bounded to 4096 candidates; on failure wifid logs the failure
and continues with the interface names supplied by the kernel. Its long-lived
control channel and netlink monitor do not gate init or system_server.

If the APEX is absent or fails activation, its rc and VINTF fragment are
absent too. The bootstrap-only Soong Wi-Fi/Bluetooth HAL, supplicant and hostapd modules are not
selected in the final product, so there is no stale vendor VINTF declaration
or init service pointing at a missing executable. Based on AOSP therefore
finishes boot with Bluetooth and Wi-Fi unavailable.

## Remains in vendor/ODM

These are device data or platform services and remain outside the APEX:

- `/vendor/etc/wifi/wpa_supplicant.conf`, which the stock supplicant reads
  from its standard device-data location;
- `/vendor/firmware/regulatory.db` and `regulatory.db.p7s`, consumed by the
  kernel regulatory loader;
- Wi-Fi and Wi-Fi Direct feature permission XML, used by PackageManager for
  capability discovery (P2P availability still follows the runtime matrix);
- `wpa_cli`, `wificond`, the framework/network stack, and the kernel modules;
- the existing Wi-Fi post-fs-data setup and hwsim modprobe service in ODM.

`libmatonos-ipc.so` is copied into the APEX because wifid links to it; other
vendor shared-library dependencies are bundled recursively by the shared
builder. No new SELinux policy file or allow rule is required: APEX file
contexts use the existing Bluetooth HAL, Wi-Fi HAL, and `matonos_driver_exec`
types. The payload root and manifest use the same `vendor_file` and
`vendor_apex_metadata_file` labels as the audio APEX.

## Build and verification

The coordinator runs the ordinary full `tools/build.sh` sequence. Pass one
installs the Soong binaries and generated inputs; the audio APEX and then radio
APEX are assembled; pass two selects both prebuilts and removes their
bootstrap binaries from the final vendor image. Do not run Soong, Gradle, or a
VM from this worktree.

Static checks for this change are `bash -n` on both APEX builders and the
shared shell helper, Python syntax compilation for `vendor-apex.py`, and
`MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh`. The attempted preflight
reached all seven checks and found no radio module collision, but was blocked
by absent prebuilt APKs in this worktree and the unavailable `bpfmt` host tool.
The refactored audio payload logic was reviewed against the pre-change builder: stage copying,
generic APEX effect extraction, sound-effect library copying, transitive
vendor dependency closure, external-library collection, XML library entries,
space-path handling, modes, manifest fields, signing arguments and compatibility
record fields are retained. A byte-for-byte payload comparison remains
unverified because this worktree has no staged PipeWire payload and the task
prohibits running the AOSP build.

After the coordinator produces a fresh image, boot it from a clean start and
run these checks:

1. In QEMU without passed-through hardware, confirm `getprop sys.boot_completed`
   reaches `1`, `cmd apexservice getActivePackages` lists
   `com.matonos.hardware.radio`, and no Bluetooth process repeatedly crashes.
   Confirm the empty-radio fallback allows `cmd wifi status` and scans to finish.
2. On the Ryzen 5800X/RX 6600 test PC with Intel 7265 Wi-Fi and Bluetooth,
   scan and connect to a WPA2/WPA3 network, verify DHCP and DNS, then pair a
   Bluetooth keyboard or headset. Confirm wifid's selected interface becomes
   `wlan0` and the Bluetooth HCI service remains responsive.
3. On the Surface Pro 3 with Marvell 88W8897 Wi-Fi/Bluetooth and Intel HDA,
   cold boot, scan/connect, verify DHCP, then pair a Bluetooth device. Repeat
   after suspend/resume and verify the same radio APEX is active.
4. On the HP ProDesk 600 G1 with no Wi-Fi/Bluetooth controller, verify boot
   completes, Bluetooth is unavailable without a crash loop, Wi-Fi scans
   return cleanly through hwsim or report unavailable, and Settings remains
   responsive. Repeat with a supported USB adapter if available.

For each boot, check `getprop sys.boot_completed`, `cmd apexservice getActivePackages`,
`service check android.hardware.bluetooth.IBluetoothHci/default`, `cmd wifi status`,
and `logcat -b crash -d`. Run
`tools/check-selinux-labels.sh` against the built image before enforcing SELinux.

## Not yet verified

No Soong build, bundle/package build, fresh boot, or physical hardware test was
run from this worktree. The new APEX's runtime linker namespace and staged
VINTF aggregation still need confirmation on the coordinator image. No kernel
change is needed.
