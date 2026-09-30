# MatonOS ODM driver bundle

The native MatonOS drivers and their runtime assets are assembled outside
Soong into `odm.img` by `tools/build-bundle.sh`. The image is read-only EROFS
and is the `odm` logical partition in the live image's dynamic-partition
group. Both fstab variants mount it at `/odm` during first-stage init. This
gives init, libvintf, SystemConfig, the linker, and property service their
standard ODM paths without a driver importer or per-driver Soong modules.

## Contents and decisions

`contents.list` is the bundle registry. `file` rows install one source at a
bundle-relative path; `tree` rows copy a whole staged tree; `prop` rows are
written to `/odm/etc/build.prop`. `build-bundle.sh` labels the image with the
shared ODM file-context rules and atomically replaces the output image.
Native executables are linked with `$ORIGIN/../lib64` RUNPATH; init service
environments also set `LD_LIBRARY_PATH=/odm/lib64` for child programs and the
PipeWire build.

The bundle contains sleepd, wifid, btd, inputd, the Bluetooth HCI HAL,
PipeWire/WirePlumber, the audio proxy, `libmatonos-ipc`, and their init rc
files. Bluetooth's HCI manifest and the MatonOS feature declarations are in
`/odm/etc/vintf/manifest` and `/odm/etc/permissions`. `property_service.cpp`
on this AOSP branch loads `/odm/etc/build.prop` as the canonical ODM property
file; this is the path the script generates.

Vendor-owned defaults formerly injected into vendor `build.prop` are in the
registry. System-owned read-only settings such as `ro.sf.lcd_density`,
`ro.logd.size`, `ro.opengles.version`, and the graphics renderer debug default
remain in their existing product configuration. PipeWire's audio policy XML,
the Wi-Fi feature declaration and regulatory database, and input
keylayout/keychar/IDC files stay in vendor: those consumers resolve them from
`/vendor/etc`, `/vendor/firmware`, or `/vendor/usr`. Bluetooth features are
read from the bundle's ODM permissions directory.

Static RROs remain in the existing overlay/product packaging. Their ODM scan
path has not been established for this product, and moving them is not needed
for the driver bundle.

New tunables should be vendor properties, runtime settings, or
`androidboot.matonos.*` loader-entry values (published as `ro.boot.matonos.*`).
Do not add new system-owned `ro.*` properties to the bundle.

## Build and static checks

`tools/build.sh` runs `tools/build-native.sh`, then `tools/build-bundle.sh`,
before the AOSP stage and live-image packaging. The bundle script needs AOSP's
host `mkfs.erofs`; it stages the bundle at
`out/target/product/pc_x86_64/odm-bundle.img` (not `odm.img` directly). AOSP
copies it as `odm.img` via `BOARD_PREBUILT_ODMIMAGE` in
`install/BoardConfig.mk`, so no AOSP-generated ODM image competes with the
bundle. `tools/make-live.sh` adds that `odm.img` to `super` as logical
partition `odm` in the same dynamic group as system, system_ext, product, and
vendor. The coordinator's `tools/quick-image.sh` does not run the AOSP copy, so
it stages the bundle and mirrors it to `odm.img` itself.

Static validation: confirm every `file`/`tree` registry source exists; every
`service` executable path in the registered rc files exists in the staging
layout; the fstab source `odm` matches the `odm` partition name passed to
lpmake; and no `matonos-*`, `maton-pipewire-proxy`, or `libmatonos-ipc`
Soong module/product references remain. Build and fresh-boot checks are
pending the coordinating agent's one full-image build.

## Install me and Updater requirements

The current live image is non-A/B. Install me must include `odm.img` in
`super` and create the `odm` logical partition using the same group and
read-only attributes as the other system partitions. For the planned A/B
layout, it must create and flash `odm_a` and `odm_b` and keep the active slot's
ODM image in sync with vendor and the system build. The Updater must write the
inactive slot's ODM image as part of one atomic OTA payload, update dynamic
partition metadata, then switch slots only after all partitions verify. The
A/B fstab entry must use slot-aware logical partition selection for
`odm_a`/`odm_b`.

## Fresh-image verification

After the coordinating agent publishes the new image, boot it fresh in QEMU
or on hardware and check:

1. `mount | grep ' /odm '` shows a read-only EROFS mount; `ls /odm/bin` and
   `ls /odm/etc/init` show the bundled drivers and rc files.
2. `getprop ro.hardware.gralloc`, `getprop ro.vendor.matonos.audio.pipewire`,
   and `getprop vendor.maton.wifi.hwsim.radios` return their ODM defaults.
3. `ps -AZ` shows MatonOS drivers in `u:r:matonos_driver:s0`; init reports no
   bad executable labels, and Bluetooth VINTF is visible to the Bluetooth
   service.
4. Exercise audio playback/capture, Wi-Fi toggle and scan, Bluetooth toggle,
   keyboard/mouse input, and suspend/resume. Repeat with unavailable devices
   where practical to confirm PipeWire's null sink and the Wi-Fi/Bluetooth
   virtual fallbacks keep boot and Android services usable.

On the Ryzen/RX 6600/Intel 7265 build PC, verify Wi-Fi and Bluetooth discovery,
HDA/HDMI audio, USB audio hotplug, keyboard/mouse input, and suspend/resume.
On the Surface Pro 3, verify Marvell Wi-Fi/Bluetooth and Intel HDA. On the HP
ProDesk 600 G1, verify Haswell Intel graphics remains independent of ODM and
that systems lacking optional radios still boot.
