# Shared changes requested

1. Add the two task-authorized first-stage image patches to the root
   `APPROVED_PATCHES` allowlist so preflight permits them:
   - `patches/system/core/0001-map-file-backed-system-images.patch`
   - `patches/system/fs/fs_mgr/0001-persist-any-by-name-block-device.patch`
   Reason: this task explicitly authorizes the small first-stage/libfiemap
   AOSP changes needed to map the new installed image-file layout. The current
   zero-patches preflight rejects both files.
2. The current worktree is missing unrelated prebuilt APK and NDK artifacts
   required by preflight; no changes requested to their owners. Populate them
   from the normal buildinfra/app build process before the shared module build.
