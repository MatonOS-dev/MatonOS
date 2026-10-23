#!/usr/bin/env bash
# build-kernel.sh: build the mainline kernel for pc_x86_64 and stage it for
# the AOSP build and make-payload.sh.
#
# Usage:
#   ./build-kernel.sh [-s <linux src>] [-a <aosp dir>] [-f <linux-firmware dir>]
#                     [-o <build dir>] [-j <jobs>] [-n] [-c] [-S]
#
#   -s  mainline kernel source    (default: $LINUX_DIR or ~/Documents/linux)
#   -a  AOSP checkout             (default: derived from this script's location)
#   -f  linux-firmware checkout   (default: ~/Documents/linux-firmware; sparse-
#                                  cloned on demand with only the needed dirs)
#   -o  kernel build (O=) dir     (default: <aosp>/out/pc-kernel)
#   -j  parallel jobs             (default: nproc)
#   -n  skip firmware staging
#   -c  configure and check the fragments only, don't build
#   -S  sign every module with the local MatonOS DEV key and require signed modules
#
# Config = kernel/base.config (broad distro config) + AOSP kernel/configs
#          android-base.config + kernel/pc.config (merged in that order,
#          later wins).
#
# Stages into <device dir>/prebuilt/ (git-ignored):
#   bzImage, kernel.config, kernel.release,
#   modules/*.ko   -> BOARD_VENDOR_KERNEL_MODULES (-> /vendor/lib/modules)
#   firmware/...   -> PRODUCT_COPY_FILES          (-> /vendor/firmware),
#                     zstd-compressed (kernel loads <name>.zst)
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARNING: $*" >&2; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
# secureboot: pinned CPU microcode is staged on every kernel build, with or without -S.
source "$DEVICE_DIR/secureboot/microcode.sh"
LINUX=${LINUX_DIR:-$HOME/Documents/linux}
FIRMWARE=${LINUX_FIRMWARE_DIR:-$HOME/Documents/linux-firmware}
KOUT=""
JOBS=$(nproc)
STAGE_FW=1
CONFIG_ONLY=0
SECURE_BOOT=0

# Android release letter + kernel branch whose android-base.config we merge.
# c/android-6.18 (Android 17) is still an empty placeholder upstream, so use
# the newest populated fragment. See NOTES.md.
ANDROID_BASE_FRAGMENT=${ANDROID_BASE_FRAGMENT:-b/android-6.12/android-base.config}
FIRMWARE_URL=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git

while getopts "s:a:f:o:j:ncSh" opt; do
  case $opt in
    s) LINUX=$OPTARG ;;
    a) AOSP=$OPTARG ;;
    f) FIRMWARE=$OPTARG ;;
    o) KOUT=$OPTARG ;;
    j) JOBS=$OPTARG ;;
    n) STAGE_FW=0 ;;
    c) CONFIG_ONLY=1 ;;
    S) SECURE_BOOT=1 ;;
    *) sed -n '2,26p' "$0"; exit 1 ;;
  esac
done

KOUT=${KOUT:-$AOSP/out/pc-kernel}
STAGE=$DEVICE_DIR/prebuilt
BASE_CFG=$AOSP/kernel/configs/$ANDROID_BASE_FRAGMENT
PC_CFG=$DEVICE_DIR/kernel/pc.config
BASE=$DEVICE_DIR/kernel/base.config
if (( SECURE_BOOT )); then
  # shellcheck source=../secureboot/secureboot.sh
  source "$DEVICE_DIR/secureboot/secureboot.sh"
  sb_ensure_dev_key
fi

# ---------------------------------------------------------------- preflight
[[ -f $LINUX/Makefile && -f $LINUX/arch/x86/configs/x86_64_defconfig ]] ||
  die "no kernel source at $LINUX (-s); e.g. git clone --depth 1 -b linux-7.2.y https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
[[ -s $BASE_CFG ]] || die "android base fragment missing or empty: $BASE_CFG"
[[ -f $PC_CFG ]]   || die "missing $PC_CFG"
[[ -f $BASE ]]     || die "missing $BASE"

PATH=$PATH:/sbin:/usr/sbin
for t in make flex bison bc perl pahole modinfo git zstd cpio; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
for h in libelf openssl; do
  pkg-config --exists "$h" 2>/dev/null || die "missing development headers for $h (libelf-dev / libssl-dev)"
done

# Prefer AOSP's prebuilt clang so kernel builds don't depend on the host
# toolchain version; fall back to the host's LLVM.
clang_dir=$(find "$AOSP/prebuilts/clang/host/linux-x86" -maxdepth 1 -name 'clang-r*' -type d 2>/dev/null | sort -V | tail -n1 || true)
cc_override=()
if [[ -n $clang_dir && -x $clang_dir/bin/clang ]]; then
  PATH=$clang_dir/bin:$PATH
  info "Using AOSP clang: $clang_dir"
  # AOSP clang warns about the host's GCC installs on every invocation. The
  # kernel's cc-option probes use -Werror, so that warning makes every probe
  # fail and silently drops STACKPROTECTOR, mitigations, etc. The kernel
  # never uses libstdc++, so the warning is irrelevant. Host tools (objtool
  # etc.) are built with -Werror too, so HOSTCC/HOSTCXX need it as well.
  nowarn=-Wno-gcc-install-dir-libstdcxx
  cc_override=(CC="clang $nowarn" HOSTCC="clang $nowarn" HOSTCXX="clang++ $nowarn")
else
  { command -v clang && command -v ld.lld; } >/dev/null || die "no clang/ld.lld found"
  info "Using host clang: $(command -v clang)"
fi
# Rust toolchain for kernel Rust (Rust binder): rustup in ~/.cargo/bin.
if [[ -x $HOME/.cargo/bin/rustc ]]; then
  PATH=$HOME/.cargo/bin:$PATH
fi
export PATH

# x86_64-v2 baseline (user, 2026-09-30). Mainline 7.2 dropped the 64-bit
# CPU-family choice (no CONFIG_MCORE2/MNEHALEM, no CONFIG_GENERIC_CPU); only
# CONFIG_X86_NATIVE_CPU (-march=native, build-machine-specific) is offered, so
# we pin v2 ourselves. kbuild appends KCFLAGS/KRUSTFLAGS after its default
# "-march=x86-64" / "-Ctarget-cpu=x86-64", so these win. Keeps the kernel
# consistent with the v2 userspace (Mesa/daemons/AOSP).
kmake() {
  make -C "$LINUX" O="$KOUT" ARCH=x86_64 LLVM=1 "${cc_override[@]}" \
    KCFLAGS="-march=x86-64-v2" KRUSTFLAGS="-Ctarget-cpu=x86-64-v2" "$@"
}

# ---------------------------------------------------------------- patches
# kernel/patches/*.patch are applied to the kernel source as uncommitted
# changes (skipped when already applied), like tools/apply-patches.sh does
# for AOSP projects.
shopt -s nullglob
for patch in "$DEVICE_DIR"/kernel/patches/*.patch; do
  if git -C "$LINUX" apply --reverse --check "$patch" 2>/dev/null; then
    info "kernel patch already applied: $(basename "$patch")"
  elif git -C "$LINUX" apply --check "$patch" 2>/dev/null; then
    git -C "$LINUX" apply "$patch"
    info "applied kernel patch: $(basename "$patch")"
  else
    die "kernel patch does not apply: $(basename "$patch")"
  fi
done
shopt -u nullglob

# ---------------------------------------------------------------- config
info "Configuring ($KOUT)"
mkdir -p "$KOUT"
"$LINUX/scripts/kconfig/merge_config.sh" -m -O "$KOUT" "$BASE" "$BASE_CFG" "$PC_CFG" >/dev/null
# secureboot: module-sign enforcement is deliberately opt-in and uses the shared MatonOS key.
if (( SECURE_BOOT )); then
  "$LINUX/scripts/config" --file "$KOUT/.config" \
    --set-str CONFIG_MODULE_SIG_KEY "$SB_KEY_DIR/kernel-signing-key.pem" \
    --enable CONFIG_MODULE_SIG --enable CONFIG_MODULE_SIG_ALL --enable CONFIG_MODULE_SIG_FORCE
fi
kmake -s olddefconfig

# Report fragment options that didn't survive olddefconfig (renamed symbols,
# unmet dependencies). pc.config mismatches are fatal; android-base ones are
# warnings because the fragment is from an older kernel branch.
check_fragment() {
  local frag=$1 fatal=$2 bad=0 line sym want got
  while IFS= read -r line; do
    if [[ $line =~ ^(CONFIG_[A-Za-z0-9_]+)=(.*)$ ]]; then
      sym=${BASH_REMATCH[1]}; want=${BASH_REMATCH[2]}
    elif [[ $line =~ ^#\ (CONFIG_[A-Za-z0-9_]+)\ is\ not\ set ]]; then
      sym=${BASH_REMATCH[1]}; want=n
    else
      continue
    fi
    got=$(grep -E "^$sym=" "$KOUT/.config" | cut -d= -f2- || true)
    [[ -z $got ]] && got=n
    if [[ $got != "$want" ]]; then
      echo "    $sym: wanted $want, got $got"
      bad=$((bad + 1))
    fi
  done < "$frag"
  if (( bad > 0 )); then
    if [[ $fatal == 1 ]]; then die "$bad option(s) from $(basename "$frag") not applied (see above)"; fi
    warn "$bad option(s) from $ANDROID_BASE_FRAGMENT not applied (see above)"
  fi
}
info "Checking android-base.config"
check_fragment "$BASE_CFG" 0
info "Checking pc.config"
check_fragment "$PC_CFG" 1
if (( SECURE_BOOT )); then
  for sym in CONFIG_MODULE_SIG CONFIG_MODULE_SIG_ALL CONFIG_MODULE_SIG_FORCE; do
    grep -qx "$sym=y" "$KOUT/.config" || die "$sym did not survive configuration; signed-module build cannot continue"
  done
  grep -Fxq "CONFIG_MODULE_SIG_KEY=\"$SB_KEY_DIR/kernel-signing-key.pem\"" "$KOUT/.config" ||
    die "kernel did not select the MatonOS Secure Boot signing key"
fi

if [[ $CONFIG_ONLY == 1 ]]; then
  info "Config OK: $KOUT/.config"
  exit 0
fi

# ---------------------------------------------------------------- build
info "Building bzImage and modules (-j$JOBS)"
kmake -j"$JOBS" bzImage modules

# ---------------------------------------------------------------- stage
krel=$(kmake -s kernelrelease)
info "Staging kernel $krel into $STAGE"
# Only remove what this script owns: prebuilt/ is shared (build-mesa.sh
# stages prebuilt/mesa there). With -n keep the previously staged firmware.
rm -rf "$STAGE/modules" "$STAGE/bzImage" "$STAGE/kernel.config" "$STAGE/kernel.release"
if [[ $STAGE_FW == 1 ]]; then
  rm -rf "$STAGE/firmware"
fi
mkdir -p "$STAGE/modules"
cp "$KOUT/arch/x86/boot/bzImage" "$STAGE/bzImage"
cp "$KOUT/.config" "$STAGE/kernel.config"
echo "$krel" > "$STAGE/kernel.release"
# secureboot: stage CPU microcode on every kernel build, regardless of -S/-n.
ucode_work=$(mktemp -d --tmpdir pc-microcode.XXXXXX)
trap 'rm -rf "$ucode_work"' EXIT
sb_build_microcode_cpio "$STAGE/microcode.cpio" "$ucode_work"
mkdir -p "$STAGE/firmware"
cp "$DEVICE_DIR/secureboot/licenses/Intel-Microcode-LICENSE.txt" "$STAGE/firmware/LICENSE.intel-microcode"
cp "$ucode_work/amd-WHENCE.txt" "$STAGE/firmware/LICENSE.amd-WHENCE"
rm -rf "$ucode_work"
trap - EXIT

modtmp=$(mktemp -d --tmpdir pc-kernel-modules.XXXXXX)
trap 'rm -rf "$modtmp"' EXIT
kmake -s INSTALL_MOD_PATH="$modtmp" INSTALL_MOD_STRIP=$((1 - SECURE_BOOT)) modules_install
# The AOSP build runs depmod itself over a flat module list.
while IFS= read -r -d '' ko; do
  name=$(basename "$ko")
  [[ -e $STAGE/modules/$name ]] && die "duplicate module name $name; flat staging can't hold both"
  cp "$ko" "$STAGE/modules/$name"
done < <(find "$modtmp/lib/modules/$krel/kernel" -name '*.ko' -print0)
info "Staged $(find "$STAGE/modules" -name '*.ko' | wc -l) modules"

# ---------------------------------------------------------------- firmware
if [[ $STAGE_FW == 1 ]]; then
  mapfile -t fw_names < <(
    find "$STAGE/modules" -name '*.ko' -exec modinfo -F firmware {} + | sort -u
  )
  info "Modules reference ${#fw_names[@]} firmware files"

  if [[ ! -d $FIRMWARE/.git ]]; then
    info "Sparse-cloning linux-firmware into $FIRMWARE"
    git clone --depth 1 --filter=blob:none --sparse "$FIRMWARE_URL" "$FIRMWARE"
  fi
  # Check out only the top-level directories our modules need.
  mapfile -t fw_dirs < <(printf '%s\n' "${fw_names[@]}" | grep / | cut -d/ -f1 | sort -u)
  if [[ -f $FIRMWARE/.git/info/sparse-checkout ]]; then
    if ((${#fw_dirs[@]})); then git -C "$FIRMWARE" sparse-checkout set "${fw_dirs[@]}"; fi
  fi

  mkdir -p "$STAGE/firmware"
  missing=0
  for fw in "${fw_names[@]}"; do
    if [[ -f $FIRMWARE/$fw ]]; then
      mkdir -p "$(dirname "$STAGE/firmware/$fw")"
      zstd -q -19 -T0 "$FIRMWARE/$fw" -o "$STAGE/firmware/$fw.zst"
    else
      missing=$((missing + 1))
    fi
  done
  for lic in "$FIRMWARE"/WHENCE "$FIRMWARE"/LICENSE.* "$FIRMWARE"/LICENCE.*; do
    if [[ -f $lic ]]; then cp "$lic" "$STAGE/firmware/"; fi
  done
  (( missing == 0 )) ||
    info "$missing referenced firmware files are not in linux-firmware (usually optional/older versions)"

  # Firmware the kernel requests by path, not via modinfo, and which lives
  # outside linux-firmware (both redistributable; distros ship them too):
  #   - SOF audio DSP firmware + topologies (sof-bin, Intel laptops ~2019+);
  #   - the signed Wi-Fi regulatory database (wireless-regdb; cfg80211
  #     requires regulatory.db + .p7s, else world-roaming channels only).
  # Pinned by version and SHA-256, cached in $FW_CACHE.
  FW_CACHE=${FW_CACHE:-$HOME/.cache/matonos/firmware}
  mkdir -p "$FW_CACHE"
  fetch_pinned() { # url sha256 -> path
    local out=$FW_CACHE/$(basename "$1")
    if ! echo "$2  $out" | sha256sum -c --status 2>/dev/null; then
      curl -sSfL -o "$out.part" "$1" || die "download failed: $1"
      echo "$2  $out.part" | sha256sum -c --status || die "SHA-256 mismatch: $1"
      mv "$out.part" "$out"
    fi
    echo "$out"
  }
  SOF_VER=2026.09.1
  sof=$(fetch_pinned "https://github.com/thesofproject/sof-bin/releases/download/v$SOF_VER/sof-bin-$SOF_VER.tar.gz" \
    42ce40ec98f366365eab8e046d779b416d80b6ff2513b8f6be2a61a88e679b73)
  REGDB_VER=2026.09.03
  regdb=$(fetch_pinned "https://mirrors.edge.kernel.org/pub/software/network/wireless-regdb/wireless-regdb-$REGDB_VER.tar.xz" \
    b22e0901227b820cd1c280abe681a15b773a5103a5e10dc442e94ebb34cbf58d)
  fwtmp=$(mktemp -d)
  tar xzf "$sof" -C "$fwtmp"
  mkdir -p "$STAGE/firmware/intel"
  # -L: PRODUCT_COPY_FILES copies regular files only, so resolve sof-bin's
  # version symlinks into real files.
  for d in "$fwtmp/sof-bin-$SOF_VER"/sof*; do
    cp -rL "$d" "$STAGE/firmware/intel/"
  done
  find "$STAGE/firmware/intel" -path '*/sof*' -type f ! -name '*.zst' -exec zstd -q -19 -T0 --rm {} \;
  cp "$fwtmp/sof-bin-$SOF_VER/LICENCE.Intel" "$STAGE/firmware/LICENCE.sof-intel"
  cp "$fwtmp/sof-bin-$SOF_VER/LICENCE.NXP" "$STAGE/firmware/LICENCE.sof-nxp"
  tar xJf "$regdb" -C "$fwtmp"
  cp "$fwtmp/wireless-regdb-$REGDB_VER"/regulatory.db{,.p7s} "$STAGE/firmware/"
  cp "$fwtmp/wireless-regdb-$REGDB_VER/LICENSE" "$STAGE/firmware/LICENSE.wireless-regdb"
  rm -rf "$fwtmp"
  info "Staged firmware: $(du -sh "$STAGE/firmware" | cut -f1)"
fi

info "Done. Kernel: $STAGE/bzImage ($krel)"
