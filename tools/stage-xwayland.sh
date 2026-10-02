#!/usr/bin/env bash
# Stage the Xwayland server and its runtime libraries from the pinned build
# (out/matonos/compositor/xstage) into the device tree for image inclusion.
# Run after build-xwayland.sh; the image imports linux/compositor/prebuilt/
# system_ext via the copy rules in device.mk.
set -Eeuo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
AOSP=$(cd "$ROOT/../../.." && pwd)
SRC=$AOSP/out/matonos/compositor/xstage/x86_64
DST=$ROOT/linux/compositor/prebuilt/system_ext
[[ -x $SRC/bin/Xwayland ]] || { echo "ERROR: Xwayland missing at $SRC/bin/Xwayland; run build-xwayland.sh" >&2; exit 1; }
mkdir -p "$DST/bin" "$DST/lib64"
install -m 0755 "$SRC/bin/Xwayland" "$DST/bin/Xwayland"
# Ship both the SONAME files and the -dev symlinks Xwayland's NEEDED list
# resolves through; ldconfig-free bionic loads exactly the NEEDED names.
find "$SRC/lib" -maxdepth 1 \( -name '*.so' -o -name '*.so.*' \) -print0 |
  while IFS= read -r -d '' lib; do
    case "$(basename "$lib")" in
      libpthread*|librt*) continue ;; # build-time stubs only
    esac
    install -m 0644 "$lib" "$DST/lib64/$(basename "$lib")"
  done
echo "Staged Xwayland runtime files under $DST:"
ls -la "$DST/bin" "$DST/lib64" | tail -n +4