# Shared changes requested: camera

- `sepolicy/matonos/service_contexts`: `android.hardware.camera.provider.ICameraProvider/external/0 u:object_r:hal_camera_service:s0` (AOSP `system/sepolicy/private/service_contexts` labels only internal/0 and virtual/0).
- `ueventd.rc`: `/dev/video* 0660 root camera` (AOSP ueventd has no video rule; default is root 0600 and the provider runs as cameraserver).

Device access itself needs no policy: AOSP labels `/dev/video[0-9]*` as `video_device` and `hal_camera` may use it.
