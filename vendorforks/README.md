# Local vendor forks

`external/drm_hwcomposer` and `external/minigbm` are built from the two local
branches below. Their fixes are committed in the AOSP project checkouts; no
AOSP patch is applied to either project. `manifest/maton.xml` pins those local
branch refs. The shallow bare snapshots under `vendorforks/repos/` are optional
recovery copies of each feature commit and are intentionally excluded from the
MatonOS repository.

| Project | Branch | Fix commit |
| --- | --- | --- |
| `external/drm_hwcomposer` | `matonos/v1.2` | `a0a805598b127214877e8acc493dadf3ca4748ff` |
| `external/minigbm` | `matonos/v1.2` | `ad5e09a4c8a3d66fd3fcf22ba41ce69760cb7d83` |

## Fix descriptions for upstream

### drm_hwcomposer: scan DRM card nodes past gaps

The wildcard device path scanner stopped at the first missing `/dev/dri/cardN`.
On PCs, simpledrm may own `card0` before the real GPU driver registers its card
as `card1` or later. Continue across missing card numbers, scanning a bounded
range of 16 nodes. This keeps the existing wildcard configuration and device
selection behavior while allowing the real DRM device to be found after a gap.
The bound prevents an unbounded filesystem scan.

Suggested upstream commit subject: `drm_hwcomposer: continue scanning DRM card nodes after gaps`.

### drm_hwcomposer: copy unsupported client targets into dumb buffers

QEMU's std VGA (`bochs-drm`) and other VRAM-helper KMS drivers cannot import
the client target dma-buf or scan out SurfaceFlinger's RGBA8888 buffers. When
the client target fails to import, copy it into a KMS-local dumb buffer and
map that instead. The copy is limited to the already-composited client target;
hardware layer imports and scan-out stay zero-copy. Follow-up commits sync the
software copy's dma-bufs, retain the KMS mapping, and replace a
`dynamic_pointer_cast` with an `IsDumbBuffer()` check so the code also builds
without RTTI. A compatibility commit adds direct `<cerrno>` includes where
`errno` is used so the build does not depend on libdrm's former transitive
include.

### drm_hwcomposer: negotiate a scan-out-able client target format

SurfaceFlinger composites the client target as RGBA_8888
(DRM_FORMAT_ABGR8888). Drivers that advertise neither ABGR8888 nor XBGR8888 on
their planes reject it, which previously forced the whole client target
through the dumb-buffer copy above. Emit a composer3 client target property
during validate so SurfaceFlinger renders the client target in a format the
display can scan out: RGBX_8888 (XBGR8888, the same bytes with alpha ignored),
then BGRA_8888 (ARGB8888), then RGB_565. Nothing is emitted when the default
is supported, so GPU-backed displays are unaffected.

Suggested upstream commit subject: `drm_hwcomposer: request a scan-out-able client target format`.

### minigbm: use Mesa GBM for the AIDL allocator and stable C mapper

Allow the AIDL allocator and stable C mapper to select the `gbm_mesa` backend
through the existing `minigbm` Soong config. With that option, both link to
`libminigbm_gralloc_gbm_mesa`; the stable C mapper also compiles its external
driver path. This routes allocations through Mesa's GBM so the generic PC
image can use Mesa-supported GPUs without a per-vendor minigbm backend.

The same fix updates the allocator AIDL interface/version and supplies the
new multi-view methods required by the current AIDL interface, returning
unsupported for allocation. It also removes the obsolete `Android.mk` wrapper
definition now superseded by the Soong module, fixes current Mesa source paths
and Soong visibility, drops an unavailable ChromeOS HBM library, and updates
the graphics common dependency. These are build integration changes needed to
consume the backend on the current AOSP branch.

Suggested upstream commit subject: `minigbm: support Mesa GBM in AIDL allocator and stable mapper`.

## Local manifest setup

The local manifest selects the feature branches in the existing AOSP project
checkouts. Install or refresh it after setting up the AOSP checkout:

```sh
cp device/maton/pc_x86_64/manifest/maton.xml .repo/local_manifests/maton.xml
```

On a fresh AOSP checkout, sync its base projects before installing this
manifest. Start drm_hwcomposer from its checked out AOSP revision. For minigbm,
fetch the BlissOS base branch. Then create each local branch and apply its
committed changeset from the AOSP root:

```sh
git -C external/drm_hwcomposer switch -c matonos/v1.2
git -C external/drm_hwcomposer am device/maton/pc_x86_64/vendorforks/changesets/drm_hwcomposer-a0a8055.patch
git -C external/minigbm remote add android-generic https://github.com/android-generic/external_minigbm
git -C external/minigbm fetch android-generic 14-x86
git -C external/minigbm switch -c matonos/v1.2 FETCH_HEAD
git -C external/minigbm am device/maton/pc_x86_64/vendorforks/changesets/minigbm-ad5e09a.patch
```

The minigbm checkout must be the BlissOS/android-generic `14-x86` base named
in the original local manifest. Install this manifest only after both feature
branches exist. On the current checkout, these branches already contain the
commits listed above. `vendorforks/changesets/` contains portable Git format
patches for recreating the branches; `apply-patches.sh` does not read them.

## Later GitHub fork setup

When publishing is approved, create GitHub forks for `drm_hwcomposer` and
`minigbm`. In each AOSP project checkout, add that fork as a remote and push
the local `matonos/v1.2` branch. Then change
`manifest/maton.xml` to use a named HTTPS remote and those pushed branch refs,
and update the setup instructions above. Preserve the local commits and review
each project's upstream license/contribution requirements before opening an
upstream change. No remote was added and nothing was pushed for this work.

## Verification and remaining work

- `git diff --check` passed before the local commits.
- Both project checkouts are clean on `matonos/v1.2`; the format-patch files
  preserve each committed change for branch recreation.
- `apply-patches.sh` discovers patches by directory contents. Removing the
  two retired patch files makes it skip both projects; all other patch paths
  remain unaffected.
- A fresh AOSP build and QEMU boot have not yet been run. They must be
  requested from the build coordinator under the workspace build rules.

Real-hardware checks after a fresh build:

1. On the Ryzen/RX 6600 PC, boot with drm debug logging and confirm
   drm_hwcomposer opens the active GPU when DRM card numbering has a gap.
2. On the same PC, confirm SurfaceFlinger starts with the Mesa GBM AIDL
   allocator and stable C mapper, then exercise display rotation, suspend/resume,
   and screenshots.
3. On the Surface Pro 3 (Intel HDA and Marvell Wi-Fi/BT) and HP ProDesk 600 G1
   (Haswell), confirm the generic Mesa GBM backend initializes and the display
   remains usable; report any formats or modifiers that fall back to client
   composition.
