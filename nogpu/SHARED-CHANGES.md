# Shared graphics changes

These shared-file changes are part of the requested no-GPU fallback:

- `graphics/pc-gpu-detect.sh`: identify PCI display adapters and their
  supported drivers before considering fallback. Give matching hardware
  render nodes a bounded 1.5-second appearance window, but keep a known GPU on
  its hardware path after the timeout. Load vgem through `/vendor/bin/modprobe`
  only when no supported PCI driver or render node exists. Reason: early-init
  can run before modalias-triggered GPU modules publish render nodes; a known
  GPU must not be diverted to llvmpipe. The bounded wait and optional module
  load keep startup finite and boot-safe.
- `sepolicy/matonos/file_contexts`: label `/dev/dri` and `/dev/dri/card*` as
  `graphics_device`, and `/dev/dri/renderD*` as `gpu_device`. Reason: the prior
  generic `device` label denied allocator access to the DRM directory/nodes.
- `sepolicy/matonos/matonos_driver.te`: allow the stock allocator, composer,
  SurfaceFlinger and Mesa GPU SP-HAL to open the corresponding DRM nodes, and
  allow the detector's existing executable domain to transition to
  `vendor_modprobe`; allow that detector to set Mesa's vendor-owned
  `vendor.mesa.*` software-rendering option. The corresponding MatonOS vendor
  property type/context is in the existing policy set. Reason: vgem must be
  opened by the stock graphics stack, loaded by the early detector, and Mesa
  must choose `kms_swrast` on enforcing builds.

No `device.mk`, root `BoardConfig.mk`, `sepolicy/vendor/*`, `tools/*`, or
other area's files are part of this change.
