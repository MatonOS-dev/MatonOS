#!/usr/bin/env bash
# Development AVB key and pinned root-vbmeta command line helpers.

AVB_KEY_DIR=${AVB_KEY_DIR:-${HOME:?HOME is required}/.config/matonos/avb}
AVB_DEV_KEY_PATH=$AVB_KEY_DIR/matonos-avb-dev.pem
AVB_KEY_PATH=${MATON_AVB_KEY_PATH:-$AVB_DEV_KEY_PATH}

avb_ensure_dev_key() {
  [[ $AVB_KEY_PATH == /* ]] || {
    echo "ERROR: AVB key path must be absolute: $AVB_KEY_PATH" >&2
    return 1
  }
  if [[ ${MATON_RELEASE:-0} == 1 && $AVB_KEY_PATH == "$AVB_DEV_KEY_PATH" ]]; then
    echo "ERROR: release packaging must set MATON_AVB_KEY_PATH to a release key" >&2
    return 1
  fi
  if [[ -e $AVB_KEY_PATH ]]; then
    [[ -s $AVB_KEY_PATH ]] || { echo "ERROR: empty AVB key: $AVB_KEY_PATH" >&2; return 1; }
    chmod 0600 "$AVB_KEY_PATH"
    return 0
  fi
  if [[ $AVB_KEY_PATH != "$AVB_DEV_KEY_PATH" ]]; then
    echo "ERROR: release/custom AVB key is missing: $AVB_KEY_PATH" >&2
    return 1
  fi
  mkdir -p "$AVB_KEY_DIR"
  chmod 0700 "$AVB_KEY_DIR"
  umask 077
  openssl genrsa -out "$AVB_KEY_PATH" 4096 >/dev/null 2>&1 || {
    rm -f "$AVB_KEY_PATH"
    echo "ERROR: could not generate development AVB key: $AVB_KEY_PATH" >&2
    return 1
  }
  chmod 0600 "$AVB_KEY_PATH"
  echo "Generated development AVB key: $AVB_KEY_PATH" >&2
}

avb_vbmeta_cmdline() {
  local image=$1 avbtool=${2:-avbtool} info size digest
  [[ -s $image ]] || { echo "ERROR: vbmeta image missing or empty: $image" >&2; return 1; }
  info=$("$avbtool" info_image --image "$image") || return 1
  size=$(awk -F: '
    /Header Block:/ { header=$2; gsub(/[^0-9]/,"",header) }
    /Authentication Block:/ { auth=$2; gsub(/[^0-9]/,"",auth) }
    /Auxiliary Block:/ { aux=$2; gsub(/[^0-9]/,"",aux) }
    END { if (header && auth && aux) print header + auth + aux }
  ' <<<"$info")
  [[ $size =~ ^[0-9]+$ && $size -gt 0 ]] || {
    echo "ERROR: could not parse vbmeta structure size from avbtool info_image" >&2
    return 1
  }
  digest=$("$avbtool" calculate_vbmeta_digest --image "$image" --hash_algorithm sha256) || return 1
  [[ $digest =~ ^[a-fA-F0-9]{64}$ ]] || {
    echo "ERROR: avbtool returned an invalid SHA-256 vbmeta digest" >&2
    return 1
  }
  printf 'androidboot.vbmeta.avb_version=1.4 androidboot.vbmeta.device_state=locked androidboot.vbmeta.hash_alg=sha256 androidboot.vbmeta.size=%s androidboot.vbmeta.digest=%s androidboot.vbmeta.invalidate_on_error=yes androidboot.veritymode=enforcing androidboot.verifiedbootstate=green' "$size" "${digest,,}"
}

avb_make_root_vbmeta() {
  local output=$1 product_out=$2 avbtool=${3:-avbtool} key=${4:-$AVB_KEY_PATH}
  local partition
  local -a args=(make_vbmeta_image --algorithm SHA256_RSA4096 --key "$key" --padding_size 4096)
  for partition in system system_ext product vendor odm; do
    [[ -s $product_out/$partition.img ]] || {
      echo "ERROR: AVB input image missing or empty: $product_out/$partition.img" >&2
      return 1
    }
    args+=(--include_descriptors_from_image "$product_out/$partition.img")
  done
  "$avbtool" "${args[@]}" --output "$output" || return 1
  # libavb reads vbmeta from a fixed maximum-size region. This pads the block
  # device image; the UKI digest helper hashes only the vbmeta structures.
  truncate -s 65536 "$output"
}
