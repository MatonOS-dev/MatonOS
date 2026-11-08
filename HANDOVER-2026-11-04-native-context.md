# Handover 2026-11-04 night: DRM native context VM

## State
- VM now boots with `-g native` (QEMU 11.1.3 + virglrenderer amdgpu-experimental).
  Guest GLES = radeonsi on host RX 6600 directly (verified via SurfaceFlinger).
- Vulkan picked `swrast` at boot: pc-gpu-detect runs at early-init before ueventd
  creates /dev/dri/renderD128. Manual run picks `radeon`. Fix (bounded 3 s wait for
  the dev node) is in graphics/pc-gpu-detect.sh — image NOT rebuilt yet (build stopped).
- drmhwc logged 4x `could not create drm fb -22` at boot only; display not confirmed.

## Next
1. `MATON_SUBSYSTEM_REPO=$HOME/matonos/repos/lsa-trim MATON_SUBSYSTEM_OUT=$HOME/matonos/build/xwayland-ahb-out MATON_BUILD_COORDINATOR=1 tools/build.sh -M -L` (NOT -K: kernel patch 0003 must be built in, else drmhwc AddFB2 -22 = black screen)
2. simg2img super.img → dd into ~/matonos/vm/disk-vm/disk.img at sector 1189888 (no conv=sparse; cmp verify)
3. Boot: `VIRGL_GBM_LAYOUT_FORCE_ENABLE=1 MATON_QEMU_DISK=nvme MATON_QEMU_PERSISTENT=1 MATON_QEMU=$HOME/matonos/qemu-venus/bin/qemu-system-x86_64 setsid nohup tools/run-qemu-live.sh -i ~/matonos/vm/disk-vm/disk.img -g native -m 12288 -r 1600x900 -a 5556 -s ~/matonos/vm/disk-vm/serial.log`
4. Check `vendor.maton.graphics.vulkan` = radeon; investigate fb -22; then vk_test, Abuse, Steam, glxinfo.

## Uncommitted (commit explicit paths only)
- Device tree: graphics/pc-gpu-detect.sh, graphics/virtgpu-caps/, graphics/Android.bp,
  graphics/graphics.mk, sepolicy/vendor/pc_gpu_detect.te, tools/run-qemu-live.sh (-g native + check),
  tools/build-mesa.sh (-Damd-options=virtio), kernel/patches/0003-drm-virtio-accept-host-modifiers.patch
- Mesa fork: src/virtio/vdrm/meson.build (dep_libdrm_amdgpu, upstream bug)
- ~/matonos/qemu-venus/build.sh (version + drm_native_context checks); QEMU src at v11.1.3,
  old plane-info patch in git stash; minigbm revert df2963d unpushed
- docs/FORKS.md: note QEMU 11.1.3 bump

## Cleanup later
TEMPDIAG lines in Xwayland, Xwayland rebuild in build script, main-out libxcb duplicate,
MATON_X11_AHB_DIR env var, trim XWAYLAND-AHB.md, maybe drop kernel 0002 + QEMU stash.
