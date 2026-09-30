#!/usr/bin/env bash
set -Eeuo pipefail
die() { echo "ERROR: $*" >&2; exit 1; }
[[ $# == 2 ]] || die "usage: $0 <fresh-live-image> <output-test-image>"
SOURCE=$(realpath "$1")
OUTPUT=$2
[[ -f $SOURCE ]] || die "source live image is missing"
[[ ! -e $OUTPUT ]] || die "output already exists: $OUTPUT"
for tool in sgdisk mcopy mtype; do command -v "$tool" >/dev/null || die "$tool is required"; done

if sgdisk -p "$SOURCE" | grep -Eq '^[[:space:]]*[34][[:space:]]'; then
  die "source already has partition 3 or 4; use a pristine live image"
fi

mkdir -p "$(dirname "$OUTPUT")"
cp --reflink=auto "$SOURCE" "$OUTPUT"
truncate -s "$(( $(stat -c %s "$OUTPUT") + 1024*1024*1024 ))" "$OUTPUT"
sgdisk -e "$OUTPUT" >/dev/null
sgdisk -n 3:0:+512M -t 3:8300 -c 3:addons_a \
       -n 4:0:+512M -t 4:8300 -c 4:addons_b "$OUTPUT" >/dev/null

ESP_START=$(sgdisk -i 1 "$OUTPUT" | awk '/First sector:/ {print $3}')
[[ $ESP_START =~ ^[0-9]+$ ]] || die "cannot find the ESP start sector"
ESP_OFFSET=$((ESP_START * 512))
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for entry in live debug; do
  mcopy -i "$OUTPUT@@$ESP_OFFSET" "::/loader/entries/matonos-$entry.conf" "$TMP/$entry.conf" \
    || die "$entry loader entry is missing from the ESP"
  if ! grep -q 'androidboot.slot_suffix=' "$TMP/$entry.conf"; then
    sed -i 's/\(androidboot.hardware=pc_x86_64\)/\1 androidboot.slot_suffix=_a/' "$TMP/$entry.conf"
  fi
  grep -q 'androidboot.slot_suffix=_a' "$TMP/$entry.conf" || die "could not add slot suffix to $entry entry"
  mcopy -o -i "$OUTPUT@@$ESP_OFFSET" "$TMP/$entry.conf" "::/loader/entries/matonos-$entry.conf"
done
sgdisk -v "$OUTPUT" >/dev/null || die "test image GPT verification failed"
echo "Prepared QEMU test image: $OUTPUT"
sgdisk -p "$OUTPUT" | tail -5
