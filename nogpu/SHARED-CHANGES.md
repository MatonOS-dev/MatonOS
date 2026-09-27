# Shared graphics changes

These shared-file changes are part of the requested no-GPU fallback:

- `graphics/pc-gpu-detect.sh`: add a bounded 1.5-second supported-render-node
  check, then attempt to load vgem through `/vendor/bin/modprobe` only when no
  supported render node appeared. Reason: vgem must exist before minigbm and
  SurfaceFlinger initialize on unsupported PCs; an unbounded wait or required
  module load could hold boot.
- `sepolicy/matonos/file_contexts`: label `/dev/dri` and `/dev/dri/card*` as
  `graphics_device`, and `/dev/dri/renderD*` as `gpu_device`. Reason: the prior
  generic `device` label denied allocator access to the DRM directory/nodes.
- `sepolicy/matonos/matonos_driver.te`: allow the stock allocator, composer,
  SurfaceFlinger and Mesa GPU SP-HAL to open the corresponding DRM nodes, and
  allow the detector's existing executable domain to transition to
  `vendor_modprobe`; allow that detector to set Mesa's `debug.mesa.*`
  software-rendering option. Reason: vgem must be opened by the stock graphics
  stack, loaded by the early detector, and Mesa must choose `kms_swrast` on
  enforcing builds.

No `device.mk`, root `BoardConfig.mk`, `sepolicy/vendor/*`, `tools/*`, or
other area's files are part of this change.
