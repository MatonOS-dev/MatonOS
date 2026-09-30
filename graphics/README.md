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

At early-init, init starts `pc-gpu-detect.sh` in the background. The bounded
detector chooses the Vulkan HAL from the primary display PCI ID and writes
`vendor.maton.graphics.vulkan`; an init property trigger sets
`ro.hardware.vulkan`, which the loader uses. It waits up to 1.5 seconds for
the selected kernel driver, then attempts a three-second vgem load and enables
Mesa software rendering if no expected render node appears. Detection errors
leave Android's boot path free to continue; the software Vulkan HAL is the
default when no GPU-specific Vulkan driver is selected.

The vgem/llvmpipe no-GPU path has not been verified on physical hardware. Its
DMA-BUF import and scanout behavior depends on the active KMS driver; keep
the boot fallback report and test checklist below current as that path is
validated.

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
