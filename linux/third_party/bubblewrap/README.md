# Bubblewrap bionic runtime test

Bubblewrap 0.10.0 is built as the `bwrap` Soong module, but the current image
does not package the resulting binary. The Flatpak spike therefore pushes the
test binary to `/data/local/tmp`.

## Bionic path fix

Patch `0003-bionic-root-bind-realpath.patch` handles Bionic `realpath()`
returning `ENOENT` for the initial `/newroot/` pivot bind. Flatpak's first
runtime launch showed the same error for prepared mount destinations beneath
`/newroot/`, such as `/newroot/usr`. Patch
`0004-bionic-newroot-subpath-realpath.patch` extends the fallback to that
internal subtree, only on `ENOENT`; the subsequent `open()` still validates
the destination. Both changes are in the patch series in
`../apply-patches.sh` and apply over pristine upstream.

The fix was cross-built with NDK r30/API 35 for x86_64 in
`out/matonos/flatpak-ndk/bubblewrap-retry-20260929/build2/bwrap` (220,152
bytes). With root and SELinux permissive on a copy of the 2026-09-29 11:42
image, both `bwrap --unshare-all --ro-bind / / true` and Flatpak's sandboxed
`sh -c 'uname -a; ls /usr; id'` succeeded. The VM used adb port 5557 and was
shut down after collecting output.

No kernel changes were needed. The next integration step is to rebuild the
Soong module with the updated patch series and package bwrap in the image.
