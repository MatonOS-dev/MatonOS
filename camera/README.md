# USB/UVC camera support (phase 1)

This enables AOSP's stock AIDL External Camera Provider for V4L2/UVC cameras. AOSP supplies the provider's init rc; the VINTF fragment is ours (BoardConfig.mk). The provider advertises the external-camera feature, and reads `vendor/etc/external_camera_config.xml`. The provider is expected to start without blocking boot when no `/dev/video*` camera exists.

AOSP already labels `/dev/video[0-9]*` as `video_device` and stock `hal_camera` policy grants access to it. Two device-tree additions are still needed: `sepolicy/matonos/service_contexts` labels `ICameraProvider/external/0` as `hal_camera_service` (AOSP labels only internal/0 and virtual/0, so registration fails and init restarts the provider), and `ueventd.rc` makes `/dev/video*` 0660 root:camera (the provider runs as cameraserver and opens nodes read/write).

The stock provider accepts MJPEG (and Z16 depth) only; YUYV-only webcams are not supported in this phase. FPS limits are a whitelist: a mode whose rates all exceed the bound is dropped, so 1080p is bounded at 30.

The stock provider already skips nodes without V4L2_CAP_VIDEO_CAPTURE (metadata nodes). The config can ignore only numeric node IDs, so this phase does not guess IDs: IR capture nodes and webcams exposing two capture nodes still show up as separate cameras until the camera-management phase. The stock provider exposes every compatible V4L2 capture node it discovers.

## Tests

No build or VM was run by the implementation agent. Coordinator verification should validate image labels and boot once without a webcam, then with a UVC webcam (QEMU usb-host passthrough by the coordinator, then real PCs). Confirm provider registration, Camera2 enumeration/preview/capture, and hotplug remove/add behavior. Check 720p/30 and 1080p modes explicitly.

## Open issues

- Stock config cannot exclude IR or duplicate capture nodes by capability; that needs node discovery outside the stock provider or a later camera HAL.
- No camera control API or stable front/back role assignment is included in phase 1.
