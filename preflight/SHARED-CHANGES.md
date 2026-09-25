# Shared change requested

File: `tools/build.sh`

Change: invoke `"$TOOLS/preflight.sh"` immediately after argument parsing and
before any kernel, Mesa, bundle, or AOSP work when the coordinator marker has
allowed the build. The preflight script itself checks
`MATON_BUILD_COORDINATOR=1` and exits non-zero on any failure.

Reason: make the static checks run automatically before every normal
coordinator build, rather than depending on a separate manual invocation.
This file is shared build infrastructure and is outside the `preflight/`
ownership boundary, so the requested edit is listed here for the coordinator.
