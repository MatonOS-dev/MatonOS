# Bubblewrap bionic fork

Current release: **0.13.0**. See `FORK-REVISION.md` and
`../source-pins.json` for archive, upstream commit and exact fork source pins.
The former patch series is retired; build the fork directly with `build.sh`
(NDK) or the existing `bwrap` Soong module (six upstream source files).

The only remaining MatonOS source change is bionic's growing POSIX getcwd
buffer. Upstream's confined destination resolution replaces both old bionic
realpath workarounds. The security path is unmodified; kernel fallbacks remain
enabled. The NDK build passes. Host namespace tests have environment limits
recorded in the r24 result report. No r24 Android device run has occurred.

Flatpak's `matonos-bwrap.c` shim now handles all upstream option arities,
including three-value `--overlay` and one-value `--overlay-src`, `--tmp-overlay`
and `--ro-overlay`. Zero-value new options use the existing zero-value default.
Direct and bundled-FD overlay argument regressions are in `bwrap_test.c`.
