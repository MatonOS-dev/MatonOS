# PipeWire camera provider: unfinished

No replacement provider is built or installed by this directory. The active
fallback remains `android.hardware.camera.provider-V1-external-service` in
`camera/camera.mk`, using `camera/external_camera_config.xml`. Do not remove
that package until its replacement compiles, packages and passes capture and
hotplug tests. No dummy provider is advertised in VINTF.

The task's source path needs correction for this checkout: the AIDL interface
is in `hardware/interfaces/camera/provider/aidl`, but the stock implementation
is in `hardware/interfaces/camera/provider/default`. It uses the v1 NDK
interface. Device/session/utility code is in `camera/device/default`.
These files belong to the clean AOSP hardware/interfaces revision
`0162af698935100a590b7359581ac8b1b80693e5` and carry Apache-2.0 headers.
No source fork or new dependency has been introduced here.

The capture conversion is unfinished, not a feature that inherently needs an
AOSP patch. Concrete missing work identified by reading the stock sources:

- `ExternalCameraProvider`: replace inotify `/dev/video*` discovery and
  `VIDIOC_QUERYCAP` checks with asynchronous PipeWire registry notifications.
  Use stable node identity plus removal handling; the global numeric ID alone
  is not stable across node replacement. Filter metadata/IR using verified
  properties and available video formats, not a guessed `/dev/videoN` number.
- `ExternalCameraDevice`: replace device opens and
  `VIDIOC_ENUM_FMT/FRAMESIZES/FRAMEINTERVALS` with PipeWire enum-param results
  before deriving Camera2 static metadata. Metadata must describe negotiated
  modes, not an invented fixed resolution/rate.
- `ExternalCameraDeviceSession`: replace `S_FMT`, `REQBUFS`, `QUERYBUF`,
  `QBUF/DQBUF`, `STREAMON/OFF` and `G/S_PARM` with a PipeWire stream,
  negotiated formats, bounded capture-request failure handling and correct
  SPA buffer ownership. Keep callbacks off locks used for Binder calls.
- `ExternalCameraUtils`: `V4L2Frame` maps offsets in the V4L2 device fd;
  it cannot be handed a PipeWire global ID or arbitrary SPA pointer. Extend
  the frame transport and preserve offline-session ownership, row strides,
  timestamps, queue return, flush and disconnect semantics. The existing
  converter accepts MJPEG and Z16; raw PipeWire webcam formats need explicit
  conversion before promising general webcam support.
- `tools/build-pipewire.sh`: currently disables both `v4l2` and `libcamera`,
  deletes their WirePlumber monitor scripts, and rejects staged V4L2 plugins.
  Both the recipe and monitor policy must change together. No libcamera SPA
  build was attempted; there is no clean-build claim or additional licence
  dependency in this task.

Switching only discovery or passing PipeWire node IDs into the stock session
would create enumerated cameras whose capture path still issues ioctls on
non-V4L2 objects. That is why `camera.mk` was not switched. No camera-service
access to PipeWire, new policy rules, APEX labels or kernel changes were
added for an absent implementation.

When resumed, fork the provider, device, session and utility sources here
with provenance, implement the transport, then package it in the media APEX
under stock `hal_camera_default` labels. Reuse the existing private PipeWire
runtime directory with explicit camera-only client permissions; do not give
Linux apps access. Startup and reconnection must be asynchronous and must
advertise an empty set when PipeWire or cameras are absent. It must never
make Android boot depend on a camera stream.

Coordinator baseline checks (fresh image, no agent USB passthrough):

```sh
adb -s <private-vm> shell getprop sys.boot_completed
adb -s <private-vm> shell service list
adb -s <private-vm> shell dumpsys media.camera
adb -s <private-vm> logcat -b all -d -s ExternalCameraProvider ExternalCameraDevice ExternalCameraDeviceSession
```

For the future replacement, also inspect PipeWire Video/Source nodes and
Camera2 enumeration, preview, JPEG capture, flush, client death and unplug
while capturing. Test MJPEG and raw-only UVC devices, metadata/IR siblings,
and a system with no webcam on each target PC. These remain unverified.
