# USB/UVC camera support (phase 1)

This enables AOSP's stock AIDL External Camera Provider for V4L2/UVC cameras. The provider uses its own AOSP init rc and VINTF fragment, advertises the external-camera feature, and reads `vendor/etc/external_camera_config.xml`. The provider is expected to start without blocking boot when no `/dev/video*` camera exists.

AOSP already labels `/dev/video[0-9]*` as `video_device`; stock `hal_camera` policy grants the provider access to that type. No device-tree SELinux additions or ueventd permissions are needed.

The stock external-camera config accepts explicit numeric video-node IDs to ignore, but has no matching by node capabilities or name. This phase therefore does not guess IDs: enumeration and filtering of IR/metadata nodes remains open for the camera-management phase. The stock provider exposes every compatible V4L2 capture node it discovers.

## Tests

No build or VM was run by the implementation agent. Coordinator verification should validate image labels and boot once without a webcam, then with a UVC webcam passed through to QEMU. Confirm provider registration, Camera2 enumeration/preview/capture, and hotplug remove/add behavior. QEMU USB webcam passthrough is required for hardware enumeration; also test on real PCs with UVC webcams.

## Open issues

- Stock config cannot exclude IR or metadata nodes by capability; implementing that needs node discovery/filtering outside the stock provider or a later camera HAL solution.
- No camera control API or stable front/back role assignment is included in phase 1.
