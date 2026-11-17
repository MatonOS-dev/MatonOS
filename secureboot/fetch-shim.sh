#!/usr/bin/env bash
# fetch-shim.sh: rebuild the committed shim.tar.xz (shim + MokManager).
# Source: Rocky Linux 10 shim-x64 16.1-2 RPM, signed by Microsoft UEFI CA 2011
# and 2023 (BSD licence). Our boot chain is trusted through MOK enrollment of
# the MatonOS key. The .efi files are committed; hashes pin what we ship.
set -Eeuo pipefail
HERE=$(dirname "$(readlink -f "$0")")
OUT=$(mktemp -d); trap 'rm -rf "$OUT" "${tmp:-}"' EXIT
RPM=shim-x64-16.1-2.el10.x86_64.rpm
URLS=(https://dl.rockylinux.org/pub/rocky/10/BaseOS/x86_64/os/Packages/s/$RPM
      https://dl.rockylinux.org/vault/rocky/10.1/BaseOS/x86_64/os/Packages/s/$RPM)
RPM_SHA=a3f186817aa5cc3687b1a35d2a27232bbf7f1d538db464a13d6ca62f8afec5e2
SHIM_SHA=8c9fcbb3f416a6ae39ded84519c61dc9743b0b90bab18907b1cc86f6ab1f932c
MM_SHA=58c53dc16f5ae3b66afdc40a089ebdd738f2a82b656da3e30c777a0068687cef
ok() { echo "$1  $2" | sha256sum -c --status 2>/dev/null; }
tmp=$(mktemp -d)
for u in "${URLS[@]}"; do curl -fsSL -o "$tmp/$RPM" "$u" && ok $RPM_SHA "$tmp/$RPM" && break; done
ok $RPM_SHA "$tmp/$RPM" || { echo "ERROR: could not fetch $RPM with the pinned hash" >&2; exit 1; }
bsdtar -xf "$tmp/$RPM" -C "$tmp" ./boot/efi/EFI/rocky/shimx64.efi ./boot/efi/EFI/rocky/mmx64.efi
mkdir -p "$OUT"
cp "$tmp/boot/efi/EFI/rocky/shimx64.efi" "$tmp/boot/efi/EFI/rocky/mmx64.efi" "$OUT/"
ok $SHIM_SHA "$OUT/shimx64.efi" && ok $MM_SHA "$OUT/mmx64.efi" || { echo "ERROR: shim/MokManager hash mismatch" >&2; exit 1; }
tar -C "$OUT" -cf - shimx64.efi mmx64.efi | xz -9e > "$HERE/shim.tar.xz"
echo "Wrote $HERE/shim.tar.xz (Rocky shim 16.1-2, CA 2011+2023)"
