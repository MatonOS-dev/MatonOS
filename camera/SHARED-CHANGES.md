# Shared changes requested: camera

No shared-file edits are required. For coordinator SELinux review, no added policy lines are requested: AOSP labels `/dev/video[0-9]*` as `video_device` (`system/sepolicy/private/file_contexts`) and stock `hal_camera` policy already grants `hal_camera` access to `video_device` (`system/sepolicy/private/hal_camera.te`, directory `r_dir_perms`, character device `rw_file_perms`). The AIDL provider's AOSP file-context rule labels its executable `hal_camera_default_exec` and its service transitions to the stock camera HAL domain.
