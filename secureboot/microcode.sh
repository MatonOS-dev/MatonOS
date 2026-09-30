#!/usr/bin/env bash
# Pinned CPU microcode sources and early-cpio assembly for x86_64 boot images.
set -Eeuo pipefail

SB_MICROCODE_DIR=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
MATON_AMD_UCODE_COMMIT=9b858e5bb58d7bf1fc4d8818cb9100aed6d46f6a
MATON_INTEL_UCODE_COMMIT=927e65c8d5a6e4ec05cc74b1778283ab2284d0c1
MATON_UCODE_CACHE=${MATON_UCODE_CACHE:-$HOME/.cache/matonos/cpu-microcode}
MATON_AMD_WHENCE_SHA256=87f8d92b2c088ac9fccb50643555f4253cb14f07ba4be137b5e0625b6176dd41

sb_ucode_repo() {
  local dest=$1 url=$2 commit=$3 sparse_dir=$4
  if [[ ! -d $dest/.git ]]; then
    mkdir -p "$(dirname "$dest")"
    git clone --depth=1 --filter=blob:none --no-checkout "$url" "$dest"
  fi
  git -C "$dest" remote set-url origin "$url"
  if ! git -C "$dest" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$dest" fetch --filter=blob:none --depth=1 origin "$commit"
  fi
  git -C "$dest" sparse-checkout init --cone
  git -C "$dest" sparse-checkout set "$sparse_dir"
  git -C "$dest" checkout --quiet --detach "$commit"
  [[ $(git -C "$dest" rev-parse HEAD) == "$commit" ]] ||
    { echo "ERROR: microcode checkout did not resolve to pinned commit $commit" >&2; return 1; }
}

sb_build_microcode_cpio() {
  local output=$1 work=$2 amd_repo=$MATON_UCODE_CACHE/linux-firmware-amd-ucode-$MATON_AMD_UCODE_COMMIT
  local intel_repo=$MATON_UCODE_CACHE/intel-microcode file
  local archive=$MATON_UCODE_CACHE/linux-firmware-amd-ucode-$MATON_AMD_UCODE_COMMIT.tar.gz
  local whence_b64=$work/amd-WHENCE.b64
  local -a amd_files=() intel_files=()
  for tool in git curl tar base64 sha256sum; do
    command -v "$tool" >/dev/null || { echo "ERROR: CPU microcode staging needs $tool" >&2; return 1; }
  done
  command -v cpio >/dev/null || { echo "ERROR: CPU microcode staging needs cpio" >&2; return 1; }
  mkdir -p "$MATON_UCODE_CACHE" "$amd_repo"
  if [[ ! -s $archive ]]; then
    curl --retry 5 --retry-all-errors --retry-delay 2 -fsSL \
      "https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+archive/$MATON_AMD_UCODE_COMMIT/amd-ucode.tar.gz" \
      -o "$archive.part"
    mv "$archive.part" "$archive"
  fi
  [[ -f $amd_repo/microcode_amd.bin ]] || tar xzf "$archive" -C "$amd_repo"
  (cd "$amd_repo" && sha256sum -c --status "$SB_MICROCODE_DIR/amd-ucode.sha256") ||
    { echo "ERROR: AMD microcode blob checksum mismatch" >&2; return 1; }
  if ! curl --retry 5 --retry-all-errors --retry-delay 2 -fsSL \
    "https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/$MATON_AMD_UCODE_COMMIT/WHENCE?format=TEXT" \
    -o "$whence_b64"; then
    # googlesource can return 503 while its pinned commit is still available
    # from the kernel-firmware GitLab mirror. Both copies are checksum-checked
    # below before any notice is bundled into a UKI.
    curl --retry 2 --retry-all-errors --retry-delay 1 -fsSL \
      "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$MATON_AMD_UCODE_COMMIT/WHENCE" \
      | base64 -w0 > "$whence_b64"
  fi
  base64 -d "$whence_b64" > "$work/amd-WHENCE.txt"
  echo "$MATON_AMD_WHENCE_SHA256  $work/amd-WHENCE.txt" | sha256sum -c --status ||
    { echo "ERROR: AMD WHENCE notice checksum mismatch" >&2; return 1; }
  sb_ucode_repo "$intel_repo" \
    https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files.git \
    "$MATON_INTEL_UCODE_COMMIT" intel-ucode

  while IFS= read -r -d '' file; do amd_files+=("$file"); done \
    < <(find "$amd_repo" -maxdepth 1 -type f -name 'microcode_amd*.bin' -print0 | sort -z)
  [[ ${#amd_files[@]} -eq $(wc -l < "$SB_MICROCODE_DIR/amd-ucode.sha256") ]] ||
    { echo "ERROR: pinned AMD microcode file set changed" >&2; return 1; }
  while IFS= read -r -d '' file; do intel_files+=("$file"); done \
    < <(find "$intel_repo/intel-ucode" -maxdepth 1 -type f \
      -name '[0-9a-f][0-9a-f]-[0-9a-f][0-9a-f]-[0-9a-f][0-9a-f]' -print0 | sort -z)
  ((${#amd_files[@]} > 0)) || { echo "ERROR: pinned AMD microcode set is empty" >&2; return 1; }
  ((${#intel_files[@]} > 0)) || { echo "ERROR: pinned Intel microcode set is empty" >&2; return 1; }

  local root=$work/microcode-root
  mkdir -p "$root/kernel/x86/microcode" "$(dirname "$output")"
  cat "${amd_files[@]}" > "$root/kernel/x86/microcode/AuthenticAMD.bin"
  cat "${intel_files[@]}" > "$root/kernel/x86/microcode/GenuineIntel.bin"
  [[ -s $root/kernel/x86/microcode/AuthenticAMD.bin &&
     -s $root/kernel/x86/microcode/GenuineIntel.bin ]] ||
    { echo "ERROR: assembled CPU microcode files are empty" >&2; return 1; }
  (cd "$root" && find . -print0 | sort -z | cpio --null -o -H newc --quiet > "$output")
  [[ -s $output ]] || { echo "ERROR: failed to create early microcode cpio" >&2; return 1; }
  echo "==> Early CPU microcode: ${#amd_files[@]} AMD and ${#intel_files[@]} Intel blobs (pinned commits)"
}
