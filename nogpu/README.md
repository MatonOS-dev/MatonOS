# No-GPU graphics fallback

MatonOS uses the upstream `vgem` DRM module as a render device when no
supported PCI GPU driver is present. Mesa's llvmpipe
renders into linear, shmem-backed dma-bufs; drm_hwcomposer presents those
frames through the PC's real KMS display device. The virtual device does not
replace the display controller.

## Design

- `graphics/pc-gpu-detect.sh` identifies PCI display hardware before checking
  render nodes. Known Intel, AMD, NVIDIA, virtio-gpu, and VMware adapters are
  treated as supported while their kernel driver is loading; the script waits
  up to 1.5 seconds for a render node matching those drivers, then leaves the
  hardware path selected even if node creation is slower. Only an unknown or
  unsupported display adapter with no other supported render node triggers
  vgem. In that case it attempts to load
  `/vendor/lib/modules/vgem.ko` with the existing vendor `modprobe`, and sets
  Mesa's Android `vendor.mesa.libgl.always.software` option so EGL selects
  `kms_swrast` for vgem instead of trying a nonexistent hardware DRI driver.
  The wait is bounded; module-load failure is ignored so boot can continue.
  Existing MatonOS property contexts and `matonos_driver.te` grant the selector
  access to this vendor-owned Mesa option under enforcing SELinux.
- minigbm's Mesa GBM backend prefers a non-vgem render node if one exists. For
  vgem it allocates linear storage from `/dev/dma_heap/system`: vgem supplies
  the Mesa DRM/PRIME import path but has no GEM buffer-creation ioctl, so
  `gbm_bo_create()` on vgem cannot allocate. The dma-bufs remain CPU-mappable
  for llvmpipe and PRIME-importable by vgem or the display KMS driver. Real
  GPU allocations keep their Mesa GBM path.
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

The 10:39 image built successfully and passed `tools/check-selinux-labels.sh`.
Fresh `-g virgl` boot reached `sys.boot_completed=1`; Mesa reported virgl,
`ro.hardware.vulkan=virtio`, the software property was unset, and vgem was not
loaded. The captured home screen is at
`/mnt/data/aosp/out/pc-logs/nogpu/virgl-1039.png`.

The 12:38 image includes the dma-heap allocator fix. Fresh `-g std` debug boot
reached `sys.boot_completed=1`; vgem was live, the software property was true,
`ro.hardware.vulkan=swrast`, and SurfaceFlinger reported llvmpipe. The render
node was labeled `gpu_device`. The captured display was black: drm_hwcomposer
logged `Unable to map KMS dumb buffer for software display copy` and
SurfaceFlinger reported `BAD_DISPLAY` on present. The client-target allocation
now works; the remaining issue was mapping the KMS dumb target through its
exported dma-buf. Patch 0004 keeps the driver's `MODE_MAP_DUMB` mapping alive
and writes the software copy through it. It is queued for a fresh build and
runtime verification. Ramfb and virgl have not yet been tested against the
12:38 image.

The dma-heap allocator and dma-buf sync changes are committed to their forks
and exported as minigbm patch 0003 and drm_hwcomposer patch 0003. The KMS
mapping lifetime fix is committed as drm_hwcomposer patch 0004. All three
compiled into the 12:38 image; the mapping lifetime fix still needs a fresh
image before it can be runtime-tested.

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

- The PCI-first detector's virgl regression was fixed and verified on image
  10:39. The 12:38 image verifies std VGA boot completion and llvmpipe, but
  its display stays black because the VRAM dumb-target copy cannot mmap the
  exported dma-buf. Verify the new KMS-fd mapping path, ramfb, and virgl on the
  next fresh image.
- The copy path supports 32-bit XRGB/ARGB/XBGR/ABGR client targets. Other
  formats or tiled modifiers return to the existing import-failure behavior.
- For recognized PCI hardware, a render node appearing after the bounded
  wait does not cause vgem to load; Android continues on the hardware path.
- No kernel configuration change is planned; `vgem.ko` is already staged in
  `prebuilt/modules`.
