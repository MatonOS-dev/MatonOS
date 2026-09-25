# Build preflight and quick image path

`tools/preflight.sh` runs fast checks before a coordinator-started AOSP build.
It checks device Soong modules and Make config for `/system` installs and
unsafe `PRODUCT_COPY_FILES`, verifies imported APK/source paths, checks the
ODM bundle registry, enforces the fixed `sepolicy/matonos/` file set and the
zero AOSP patch rule, and runs `bpfmt -l` on device `Android.bp` files. It
prints each failure with a source path and exits non-zero. It needs Python 3
and the AOSP host `bpfmt` tool. Run it with
`MATON_BUILD_COORDINATOR=1 tools/preflight.sh`.

`tools/quick-image.sh` bypasses Soong analysis only when configuration inputs
and the build environment match the ones used to generate the existing graph.
It runs the existing combined graph's system, system_ext, vendor, and product
image targets with Ninja, then rebuilds `odm.img` from
`bundle/contents.list` and repacks the live image. It refuses stale or missing
graphs, configuration inputs newer than the Soong graph, environment changes,
and a concurrent build. A changed `Android.bp`, `.mk`, product config, or
build-affecting environment requires the full coordinator build path.

The `--bundle-only` path does no AOSP build and only rebuilds `odm.img` plus
the live image from the existing partition images. Use
`MATON_BUILD_COORDINATOR=1 tools/quick-image.sh --bundle-only --dry-run` to
check its inputs and see the exact commands without changing files. Both
scripts require the coordinator marker because they are part of the shared
build workflow.

## Checks and limitations

The static scanner deliberately treats AOSP libraries in device
`PRODUCT_PACKAGES` as unsafe: their module may also install a `/system` copy.
Use an explicit vendor module or a local prebuilt with a fixed partition.
VINTF fragments are runtime ODM bundle content (or a board manifest input),
never `PRODUCT_COPY_FILES`. New MatonOS SELinux policy types must fit in the
four existing files; a fifth file changes Soong's analysis inputs.

The fast path only reuses a graph that already exists. It does not replace the
full build after configuration changes, and it does not build the kernel,
Mesa, Gradle apps, or NDK daemons. Run the full build script for those inputs.

## Verification

Run the shell syntax checks and the coordinator-gated static/dry-run checks:

```sh
bash -n tools/preflight.sh tools/quick-image.sh
MATON_BUILD_COORDINATOR=1 tools/preflight.sh
MATON_BUILD_COORDINATOR=1 tools/quick-image.sh --bundle-only --dry-run
```

The static scanner is read-only. The bundle dry run validates that the four
partition images and registry exist, then prints the bundle and repack
commands; it does not invoke them or modify image outputs. A full build and
fresh QEMU boot remain the coordinator's verification steps.

The current checkout's static run reports two existing formatting failures:
`apps/Android.bp` and `systembridge/Android.bp` are listed by `bpfmt -l`.
Those files belong to other areas; the coordinator should have their owners
format them before treating preflight as clean. The fixed SELinux allowlist
includes the already existing `matonos_bridge.te` alongside the driver policy
files.
