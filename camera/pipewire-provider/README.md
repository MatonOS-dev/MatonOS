# PipeWire camera provider

This directory now contains a provider/device/session/converter fork and a
working transport implementation, rather than a discovery-only placeholder.
The stock provider remains the **default selected package** until the
coordinator verifies the final platform link and packaging. Opt in with
`MATON_PIPEWIRE_CAMERA=true` in both the product build and media APEX packer;
never install/start both implementations of `external/0`.

`PipeWireCamera.c` starts/reconnects asynchronously, observes the registry and
node EnumFormat parameters, and accepts `Video/Source` MJPEG/YUY2/NV12 modes.
Static metadata uses the actual enumerated dimensions/rates (explicit enum
choices and valid defaults of ranges, without inventing a resolution table).
Gray/depth/metadata-only formats and obvious IR/metadata descriptions are
filtered. Stable node names yield stable numeric Camera2 IDs; an identity
hash collision logs and refuses the second camera rather than aliasing it.
Node generations detect unplug/replacement even when a polling interval
contains both removal and re-addition. A provider worker publishes status
changes outside PipeWire locks and retries incomplete initialization.

Camera roles use the numeric Camera2 ID (the hash of PipeWire's node name).
PipeWire's V4L2 node names derive from the udev port path, so a role follows
the camera attached to that port across reconnects; moving it to another port
can give it a different ID.

The `persist.matonos.camera.front` and `.back` properties select FRONT/BACK
metadata; other cameras remain EXTERNAL. A property change makes the provider
announce NOT_PRESENT then PRESENT so Camera2 reloads characteristics. Settings
changes go through the System Bridge `camera` operation. The property is a
system-public SELinux type: only the bridge sets it, while `hal_camera_default`
reads it. Stock app-domain property-set neverallows remain intact. This path
has not been built or runtime-tested in this scoped change.

Each session negotiates a source stream, copies the latest frame into owned
CPU storage and immediately queues its SPA buffer back. YUYV and single- or
two-plane NV12 are packed with source stride/offset/size checks before the
stock libyuv converter; MJPEG follows the stock decode path. Owned frame
storage also supports the inherited offline-session copying. Capture waits
at most two seconds and wakes on stop/disconnect. Stream configuration and
provider startup never wait for a camera or PipeWire connection. The empty
registry therefore reports no cameras while the rest of the system boots.

The recipe enables V4L2 SPA and keeps its WirePlumber monitor. The server and
monitor get camera-device access; the provider uses stock
`hal_camera_default` labels and joins the audio-owned private runtime as the
audio group. No Linux app gets system PipeWire access. The optional APEX
payload contains the executable, init/config and an override/updatable VINTF
entry for the existing external/0 instance. No AOSP patch or kernel change.

Validation: `check-native.py --aosp AOSP --ndk NDK --pipewire-prefix PREFIX
--out OUTPUT` regenerates frozen v1 camera interfaces and compiles all seven
C++ files plus the C transport to NDK/API-35 objects. It passed with warnings
as errors using the checkout's other generated platform headers. The V4L2
core plugin linked locally; its udev monitor also passed a separate NDK
syntax check with the pinned libudev-zero header. The final provider link,
full stack build, policy/image/APEX checks and runtime behavior are unverified.
This is why the stock provider stays selected by default.

Coordinator/device tests:

1. Build/link `matonos-camera-provider`, then opt in for the full image/APEX.
   Fresh boot without cameras and with PipeWire absent/failing must complete;
   `dumpsys media.camera` should show an empty external provider, without a
   restart loop or boot prerequisite.
2. On each target PC (Ryzen, Surface Pro 3, Haswell), enumerate cameras and
   negotiated modes. Check visible cameras while IR/metadata siblings stay
   hidden. Use a raw-only YUYV webcam, an NV12 camera and an MJPEG UVC camera.
3. Open Camera2 preview/JPEG, change resolution/FPS, close/reopen, kill the
   client, flush and exercise offline capture. Check orientation, colors,
   stride handling, timestamps and frame errors.
4. Unplug while capturing, reconnect and verify stable IDs and source
   replacement. Restart PipeWire and check removal/recovery with no stale
   usable sessions. Verify another camera can still be opened.
5. Inspect AVCs and verify a Linux stub UID cannot connect to the private
   system PipeWire socket. VMs may use coordinator-provided emulated cameras;
   do not pass through host USB/VFIO devices.

libcamera and Roc are optional and were not built. Remaining work is final
platform integration/runtime verification, not an established AOSP limitation.
