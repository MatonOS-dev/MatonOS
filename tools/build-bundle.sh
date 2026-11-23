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
AUDIO_PIPEWIRE_HAL=${MATON_AUDIO_PIPEWIRE_HAL:-true}
case $AUDIO_PIPEWIRE_HAL in
  true|false) ;;
  *) die "MATON_AUDIO_PIPEWIRE_HAL must be true or false" ;;
esac

# Generate boot assets before consuming the bundle rows that install them.
python3 "$DEVICE_DIR/bootanim/generate.py" --out "$DEVICE_DIR/bootanim/out"

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
      # The PipeWire HAL init definition overrides the stock APEX service, so
      # keep it out of an explicit BayLibre fallback bundle.
      if [[ $source == audio/native-hal/matonos-audio-hal.rc && $AUDIO_PIPEWIRE_HAL != true ]]; then
        continue
      fi
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

# ODM audio policy has priority over /vendor/etc. Select the exact policy pair
# used by audio/audio.mk so the image never loads a stale backend's primary.
if [[ $AUDIO_PIPEWIRE_HAL == true ]]; then
  audio_policy_dir=$DEVICE_DIR/audio/pipewire-hal
else
  audio_policy_dir=$DEVICE_DIR/audio/config-odm
fi
for policy_file in audio_policy_configuration.xml primary_audio_policy_configuration.xml; do
  policy_source=$audio_policy_dir/$policy_file
  if [[ $AUDIO_PIPEWIRE_HAL == false && $policy_file == primary_audio_policy_configuration.xml ]]; then
    policy_source=$DEVICE_DIR/audio/config-odm/primary_audio_policy_configuration.xml
  fi
  [[ -f $policy_source ]] || die "audio policy input missing: $policy_source"
  install -m 0644 "$policy_source" "$root/etc/$policy_file"
done
printf 'ro.vendor.matonos.audio.pipewire=%s\n' "$AUDIO_PIPEWIRE_HAL" >> "$root/etc/build.prop"

# A separate inode for the stock module-loader applet. Labeling the shared
# vendor toolbox would also transition cat/setprop/readlink in the detector.
# Keep its basename modprobe so toolbox dispatches only the module loader.
[[ -x $PRODUCT_OUT/vendor/bin/toolbox ]] || die "vendor toolbox missing (fresh tree: tools/build.sh -m libbinder_ndk.vendor,toolbox_vendor first)"
mkdir -p "$root/bin"
install -m 0755 "$PRODUCT_OUT/vendor/bin/toolbox" "$root/bin/modprobe"

# Bundle executables live in /odm/bin and use $ORIGIN/../lib64. Keep the
# runtime path explicit as well as setting LD_LIBRARY_PATH in service rc files.
if command -v patchelf >/dev/null 2>&1; then
  while IFS= read -r -d '' binary; do
    # Scripts are executable too; only ELF files take an rpath.
    [[ $(head -c4 "$binary") == $'\x7fELF' ]] || continue
    patchelf --set-rpath '$ORIGIN/../lib64' "$binary"
  done < <(find "$root/bin" -type f -perm /111 -print0 2>/dev/null || true)
fi

tmp_image=$work/odm.img
"$MKFS_EROFS" --all-root --mount-point=/odm \
  --file-contexts="$DEVICE_DIR/sepolicy/matonos/file_contexts" \
  -zlz4 "$tmp_image" "$root"
[[ -s $tmp_image ]] || die "mkfs.erofs produced an empty image"
# AOSP copies BOARD_PREBUILT_ODMIMAGE verbatim, and vbmeta includes odm's
# descriptors (fstab: avb), so the bundle carries its own hashtree footer.
AVBTOOL=$HOSTBIN/avbtool
[[ -x $AVBTOOL ]] || die "AOSP host avbtool missing: $AVBTOOL"
image_size=$(stat -c %s "$tmp_image")
partition_size=$(( (image_size + image_size / 50 + 1048576 + 4095) / 4096 * 4096 ))
"$AVBTOOL" add_hashtree_footer --image "$tmp_image" --partition_name odm --hash_algorithm sha256 \
  --partition_size "$partition_size" --do_not_generate_fec
chmod 0644 "$tmp_image"
mv -f "$tmp_image" "$OUT"
echo "ODM driver image ready: $OUT ($(du -h "$OUT" | awk '{print $1}'))"
