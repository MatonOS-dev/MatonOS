#!/usr/bin/env bash
# Assemble MatonOS' ODM driver payload outside Soong.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT_OUT=$AOSP/out/target/product/pc_x86_64
OUT=$PRODUCT_OUT/odm.img
while getopts 'o:h' opt; do
  case $opt in
    o) OUT=$OPTARG ;;
    *) echo "Usage: $0 [-o output-odm.img]"; exit 1 ;;
  esac
done

HOSTBIN=$AOSP/out/host/linux-x86/bin
MKFS_EROFS=$HOSTBIN/mkfs.erofs
[[ -x $MKFS_EROFS ]] || die "AOSP host mkfs.erofs missing: $MKFS_EROFS"
LIST=$DEVICE_DIR/bundle/contents.list
[[ -s $LIST ]] || die "bundle registry missing: $LIST"

mkdir -p "$(dirname "$OUT")"
work=$(mktemp -d --tmpdir="$(dirname "$OUT")" .odm-bundle.XXXXXX)
trap 'rm -rf "$work"' EXIT
root=$work/root
mkdir -p "$root"
mkdir -p "$root/etc"
cat > "$root/etc/build.prop" <<'EOF'
# MatonOS driver bundle properties. Source: bundle/contents.list.
EOF

while IFS=$' \t' read -r kind source destination extra; do
  [[ -n ${kind:-} && $kind != \#* ]] || continue
  case $kind in
    file)
      [[ -n ${source:-} && -n ${destination:-} && -z ${extra:-} ]] ||
        die "bad file row in $LIST: $kind $source $destination $extra"
      [[ -f $DEVICE_DIR/$source ]] || die "bundle input missing: $source"
      [[ $destination != /* && $destination != *..* ]] || die "unsafe bundle path: $destination"
      mkdir -p "$root/$(dirname "$destination")"
      install -m "$(stat -c '%a' "$DEVICE_DIR/$source")" "$DEVICE_DIR/$source" "$root/$destination"
      ;;
    tree)
      [[ -n ${source:-} && -n ${destination:-} && -z ${extra:-} ]] ||
        die "bad tree row in $LIST: $kind $source $destination $extra"
      [[ -d $DEVICE_DIR/$source ]] || die "bundle input directory missing: $source"
      [[ $destination != /* && $destination != *..* ]] || die "unsafe bundle path: $destination"
      mkdir -p "$root/$destination"
      cp -a "$DEVICE_DIR/$source/." "$root/$destination/"
      ;;
    prop)
      [[ -n ${source:-} && -z ${destination:-} && -z ${extra:-} &&
         $source == *=* && $source != *$'\n'* ]] || die "bad property row in $LIST"
      key=${source%%=*}
      [[ $key =~ ^[a-zA-Z0-9._-]+$ ]] || die "invalid property name: $key"
      printf '%s\n' "$source" >> "$root/etc/build.prop"
      ;;
    *) die "unknown registry row in $LIST: $kind" ;;
  esac
done < "$LIST"

# Bundle executables live in /odm/bin and use $ORIGIN/../lib64. Keep the
# runtime path explicit as well as setting LD_LIBRARY_PATH in service rc files.
if command -v patchelf >/dev/null 2>&1; then
  while IFS= read -r -d '' binary; do
    patchelf --set-rpath '$ORIGIN/../lib64' "$binary"
  done < <(find "$root/bin" -type f -perm /111 -print0 2>/dev/null || true)
fi

tmp_image=$work/odm.img
"$MKFS_EROFS" --all-root --mount-point=/odm \
  --file-contexts="$DEVICE_DIR/sepolicy/matonos/file_contexts" \
  -zlz4 "$tmp_image" "$root"
[[ -s $tmp_image ]] || die "mkfs.erofs produced an empty image"
chmod 0644 "$tmp_image"
mv -f "$tmp_image" "$OUT"
echo "ODM driver image ready: $OUT ($(du -h "$OUT" | awk '{print $1}'))"
