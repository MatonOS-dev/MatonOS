# Camera provider provenance

`src/ExternalCamera*`, `src/convert.*`, and `src/external-service.cpp` were
forked from AOSP `hardware/interfaces`, commit
`0162af698935100a590b7359581ac8b1b80693e5`:

- `camera/provider/default/ExternalCameraProvider.{h,cpp}` and
  `external-service.cpp`;
- `camera/device/default/ExternalCameraDevice.{h,cpp}`,
  `ExternalCameraDeviceSession.{h,cpp}`, `ExternalCameraOfflineSession.{h,cpp}`,
  `ExternalCameraUtils.{h,cpp}` and `convert.{h,cpp}`.

Original Apache-2.0 notices are preserved. Changes replace device discovery,
mode enumeration, stream configuration/capture and frame ownership with
PipeWire; retain Camera2 request/metadata, gralloc, JPEG/conversion and offline
session code. Historical V4L2 type/method names remain to minimize changes;
they do not open V4L2 devices or issue ioctls. The MJPEG format label in static
metadata is the inherited color-format bucket, not a claim that raw frame
bytes contain JPEG. Frames carry their actual fourcc into conversion.

New `PipeWireCamera.{c,h}`, `LogClose.h` and compile helper: Apache-2.0.
PipeWire/SPA and WirePlumber: MIT, pins in `audio/pipewire-PROVENANCE.md`.
The V4L2 SPA plugin is part of that same PipeWire pin. libudev-zero is ISC at
its existing pin. libcamera was not added or tested; V4L2 serves UVC webcams.

Inherited linked conversion dependencies: libyuv (BSD-3-Clause),
libjpeg-turbo (IJG/BSD-3-Clause/zlib notices), libexif (LGPL-2.1-or-later,
select 2.1). Platform Binder/HIDL, camera metadata/common helper, fmq,
gralloc/UI, logging/base/cutils/utils and tinyxml2 retain their AOSP
Apache-2.0/BSD/MIT notices. No new (L)GPLv3 dependency is introduced. MPL-2.0
is allowed by the APEX rule; permissive-only is not the rule.
