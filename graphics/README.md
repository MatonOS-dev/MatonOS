# PC graphics

MatonOS selects the Vulkan HAL from the primary PCI display adapter in
`pc-gpu-detect.sh`. Mesa EGL/GLES selects its driver from the render node,
so the same image can use a PC's native Mesa driver or virgl in QEMU. If no
supported GPU is present, the script enables the vgem/llvmpipe software
fallback. GPU discovery is bounded and does not block boot.

## Virgl QEMU check (2026-09-30)

The reported hang after the systemd-boot menu was tested on the 10:06 image
using a copy with the loader default set to `matonos-debug.conf`:

```sh
MATON_QEMU_HEADLESS=1 tools/run-qemu-live.sh \
  -i /mnt/data/aosp/out/pc-logs/virgl-fix/image-debug-copy-1006.img \
  -g virgl -m 4096 -a 5569 \
  -s /mnt/data/aosp/out/pc-logs/virgl-fix/serial-debug.log
```

The issue did not reproduce with the agent VM's `egl-headless` display. The
kernel detected virtio-vga with `+virgl +edid`, initialized virtio_gpu, and
made virtio_gpudrmfb the primary framebuffer. hwcomposer-3 and SurfaceFlinger
started; `sys.boot_completed=1` arrived at about 35 seconds. SurfaceFlinger
reported `GLES: Mesa, virgl` on the host AMD Radeon RX 6600 with Mesa 26.2.3.
A keyguard screenshot rendered successfully after enabling a temporary PIN
in the disposable guest.

The serial log, selected graphics boot events, SurfaceFlinger dump, guest
properties and screenshot are in `/mnt/data/aosp/out/pc-logs/virgl-fix/`.
No graphics configuration change was made because this run provides no
failure to fix. The windowed GTK path remains unverified: agent VMs are
forced to `egl-headless` by the VM rule.

## GPU detection and fallback

At early-init, init runs `pc-gpu-detect.sh` synchronously. This completes
before the later core-service start actions, including SurfaceFlinger and the
allocator. The bounded detector chooses the Vulkan HAL from the primary
display PCI ID and writes `vendor.maton.graphics.vulkan`; an init property
trigger sets `ro.hardware.vulkan`, which the loader uses. It also publishes
`vendor.maton.graphics.{vendor,device,driver,render_node,mode}` using the
existing `vendor.maton.` property context and `vendor_maton_prop` type. These
properties report PCI IDs, the DRM driver, `/dev/dri/renderD*` (or `unknown`),
and `hardware` or `software` rendering mode. It waits at most 1.5 seconds for
the selected kernel driver, then attempts a three-second vgem load and enables
Mesa software rendering if no expected render node appears. Detection errors
leave Android's boot path free to continue; the software Vulkan HAL is the
default when no GPU-specific Vulkan driver is selected.

The vgem/llvmpipe no-GPU path has not been verified on physical hardware. Its
DMA-BUF import and scanout behavior depends on the active KMS driver; keep
the boot fallback report and test checklist below current as that path is
validated.

The device tree's only GPU PCI metadata reader is this detector. Mesa, minigbm,
drm_hwcomposer, boot animation, and AOSP framework implementations are staged
or built outside this tree, so their source-level sysfs behavior could not be
changed here. The former policy grants show that `bootanim`, the graphics
allocator, SurfaceFlinger, `system_server`, and platform apps had GPU sysfs
read access. They now have no such grant; any remaining direct reads will
produce SELinux denials and need source/configuration changes in the owning
component. Check AVCs on the debug-permissive real-hardware boot.

## Real hardware checks

1. Boot on a PC with an Intel, AMD or NVIDIA GPU and confirm Android reaches
   the lock screen.
2. Run `dumpsys SurfaceFlinger` and confirm GLES reports the expected Mesa
   driver; take a screenshot to confirm composition.
3. Repeat with a second supported GPU if available. Confirm the primary GPU
   selection follows the boot display adapter.
4. Boot with no supported GPU and confirm the vgem/llvmpipe fallback reaches
   the lock screen without delaying boot.
5. Plug or unplug an external display and confirm the active display remains
   usable.

## Virtio-GPU Venus AHardwareBuffer support (2026-10-19)

On the running Venus VM (`127.0.0.1:5556`), the active properties are
`ro.hardware.gralloc=minigbm_gbm_mesa` and `ro.hardware.vulkan=virtio`.
The AIDL allocator is the `minigbm` service; it and the stable mapper link the
same `libminigbm_gralloc_gbm_mesa`. The fork's runtime dispatch recognizes the
DRM driver name `virtio_gpu`, tries minigbm's native `virtgpu` backend first,
and falls back to GBM Mesa if virtgpu initialization fails. Other DRM drivers
continue directly through GBM Mesa. `pc-gpu-detect.sh` still selects Vulkan
from the virtio PCI ID; gralloc dispatch is independent of Vulkan selection.

An instrumented native probe allocated a 500x500 RGBA8 AHB with usage `0xb00`
(GPU color output, sampled image and compositor overlay); stride was 500.
AHardwareBuffer exposes usage and layout in its description, but no blob flag.
The native backend creates shareable blob resources when the host advertises
blob and context-init support. Allocator and mapper use the same library and
the same driver-name dispatch, so Android hardware buffers are created and
imported by matching backends. The GBM Mesa wrapper remains packaged for
fallback and for non-virtio GPUs.

Validation for this change: build the allocator service, stable mapper and
`gralloc.minigbm_gbm_mesa` from the `matonos/v26.10` fork, then bind-mount the
rebuilt service binary and mapper library into the existing Venus VM without
rebooting. Allocate an AHB and confirm native probe property retrieval/import,
run the stub's `vkcube` and capture two distinct frames, and capture the
launcher plus an Android app to confirm normal composition. Check the native
backend selection in logs and confirm the AHB path uses a virtio blob. Test
real AMD/Intel/NVIDIA and no-GPU fallback on freshly built images before
release; those environments are not covered by Venus VM testing.
