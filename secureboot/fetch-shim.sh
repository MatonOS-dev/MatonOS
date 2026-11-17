#!/usr/bin/env bash
# fetch-shim.sh: stage MatonOS's shim + MokManager.
# Source: the dual-signed (Microsoft UEFI CA 2011 + 2023) shim 16.1 that
# Ventoy 1.1.18 ships (Rocky Linux build, BSD-2/3 licence). Our boot chain is
# trusted through MOK enrollment of the MatonOS key. Binaries are never
# committed; hashes pin exactly what we ship.
set -Eeuo pipefail
OUT=${1:-$HOME/.cache/matonos/shim}
VER=1.1.18
URL=https://github.com/ventoy/Ventoy/releases/download/v$VER/ventoy-$VER-linux.tar.gz
TGZ_SHA=d86ff9de63d94c8b8f1f6b14d23c55af4b88e8207bf2893890892a6a97b1ad54
SHIM_SHA=1ff3f223c2fcf5b11615d042fcb5674c4651bbbc8505b5b2987d60da0cb65d1a
MM_SHA=1a3687f923d077080fe49feb470e3932c2b1d3fd4c6439123aa0226246a24522
if [[ -f $OUT/shimx64.efi && -f $OUT/mmx64.efi ]] &&
   echo "$SHIM_SHA  $OUT/shimx64.efi" | sha256sum -c --status &&
   echo "$MM_SHA  $OUT/mmx64.efi" | sha256sum -c --status; then exit 0; fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/v.tgz" "$URL"
echo "$TGZ_SHA  $tmp/v.tgz" | sha256sum -c --status || { echo "ERROR: Ventoy tarball hash mismatch" >&2; exit 1; }
tar -xzf "$tmp/v.tgz" -C "$tmp" --wildcards "*ventoy-$VER/ventoy/ventoy.disk.img.xz"
xz -dc "$(find "$tmp" -name ventoy.disk.img.xz)" > "$tmp/efi.img"
mkdir -p "$OUT"
mcopy -n -i "$tmp/efi.img" ::/EFI/BOOT/BOOTX64.EFI "$OUT/shimx64.efi"
mcopy -n -i "$tmp/efi.img" ::/EFI/BOOT/MMX64.EFI "$OUT/mmx64.efi"
echo "$SHIM_SHA  $OUT/shimx64.efi" | sha256sum -c --status || { echo "ERROR: shim hash mismatch" >&2; exit 1; }
echo "$MM_SHA  $OUT/mmx64.efi" | sha256sum -c --status || { echo "ERROR: MokManager hash mismatch" >&2; exit 1; }
echo "Staged shim 16.1 (CA 2011+2023) in $OUT"
