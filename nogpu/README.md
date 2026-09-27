# No-GPU graphics fallback

MatonOS uses the upstream `vgem` DRM module as a render device when no
supported hardware render node appears during early boot. Mesa's llvmpipe
renders into linear, shmem-backed dma-bufs; drm_hwcomposer presents those
frames through the PC's real KMS display device. The virtual device does not
replace the display controller.

## Design

- `graphics/pc-gpu-detect.sh` gives PCI graphics drivers up to 1.5 seconds to
  publish a supported render node. If none appears, it attempts to load
  `/vendor/lib/modules/vgem.ko` with the existing vendor `modprobe`, and sets
  Mesa's Android `debug.mesa.libgl.always.software` option so EGL selects
  `kms_swrast` for vgem instead of trying a nonexistent hardware DRI driver.
  The wait is bounded; module-load failure is ignored so boot can continue.
  Existing `matonos_driver.te` grants this selector the debug-property write
  permission needed under enforcing SELinux.
- minigbm's Mesa GBM backend prefers a non-vgem render node if one exists. For
  vgem it requests linear, non-scanout buffers and leaves render/texture use
  enabled for the software renderer. Real GPU allocations keep their previous
  behavior.
- drm_hwcomposer first imports buffers normally. If import fails for the
  already-composited HWC3 client target, it waits up to three seconds for the
  acquire fence and copies a supported 32-bit linear buffer into a KMS dumb
  buffer. Other layers and drivers with dma-buf import support retain the
  normal zero-copy path.
- Existing SELinux policy files label DRM render nodes as `gpu_device`, KMS
  card nodes as `graphics_device`, and grant the stock graphics components the
  access needed to open them. The detector can invoke the narrowly scoped
  module-loader transition.

## Build and test status

The initial implementation built and passed the SELinux label check. Its first
fresh std-VGA boot confirmed that vgem loads and Vulkan selects `swrast`, but
SurfaceFlinger crash-looped. Based on Mesa's Android loader path, the likely
cause is that EGL tried hardware DRI instead of `kms_swrast`. The detector now
sets Mesa's existing Android software-rendering option when no supported
render node is found. That code and its policy permission are queued for a
rebuild and fresh-boot verification; the fallback is not yet working end to
end.

## Fresh-image tests

1. Boot QEMU with `tools/run-qemu-live.sh -g std -a 5563 -s
   /mnt/data/aosp/out/pc-logs/nogpu/serial.log`. Confirm `sys.boot_completed=1`,
   `/dev/dri/renderD*` is labeled `gpu_device`, the render node reports driver
   `vgem`, SurfaceFlinger reports `llvmpipe`, and `vulkan.swrast.so` is the
   selected Vulkan HAL. Capture the screen and confirm the MatonOS home screen
   is visible.
2. Repeat with a QEMU bochs display and with ramfb, using the headless
   `-g none -x "-device bochs-display"` or `-x "-device ramfb"` options.
   Confirm the dumb-buffer copy path presents the home screen.
3. Boot with `-g virgl`. Confirm vgem is not loaded and SurfaceFlinger still
   reports the virgl renderer.
4. On the Ryzen/RX 6600 build PC, Surface Pro 3, and HP ProDesk 600 G1, boot
   freshly and check `sys.boot_completed`, `ls -lZ /dev/dri`,
   `dumpsys SurfaceFlinger | grep GLES`, and `test -d /sys/module/vgem`. Confirm
   the hardware renderer is selected, vgem is absent, and display output is
   unchanged. The Surface Pro 3 has Marvell Wi-Fi/BT and Intel HDA; the HP is
   Haswell. Use the QEMU no-renderer cases above to verify the software
   fallback because all three physical test PCs have supported graphics.

Use a fresh image for each run. Do not remount, push files over system files,
or restart framework services to test.

## Open issues

- The Mesa software-rendering property fix, vgem EGL/GBM path, and HWC3
  dumb-buffer fallback still need a new build and fresh-boot verification
  across the matrix above.
- The copy path supports 32-bit XRGB/ARGB/XBGR/ABGR client targets. Other
  formats or tiled modifiers return to the existing import-failure behavior.
- The early detector's 1.5-second wait is a compromise for late PCI modules;
  hardware whose driver appears after that timeout may transiently load vgem.
- No kernel configuration change is planned; `vgem.ko` is already staged in
  `prebuilt/modules`.
