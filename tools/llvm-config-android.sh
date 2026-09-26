#!/usr/bin/env bash
# llvm-config-android.sh: host-runnable llvm-config for an LLVM cross-built
# for Android (out/pc-mesa/llvm-android, see build-mesa.sh). Mesa's meson
# finds LLVM only through llvm-config, and the cross-built one can't run on
# the host, so answer from the host's llvm-config of the SAME version with
# paths, components and system libs rewritten for the Android install.
#
# Env: MATON_LLVM_ANDROID_PREFIX  the Android LLVM install prefix
#      MATON_LLVM_HOST_CONFIG     host llvm-config (default llvm-config-21)
set -euo pipefail
P=${MATON_LLVM_ANDROID_PREFIX:?set MATON_LLVM_ANDROID_PREFIX}
HOST=${MATON_LLVM_HOST_CONFIG:-/usr/lib/llvm-21/bin/llvm-config}
host_prefix=$("$HOST" --prefix)

# Components: the host's list minus other targets' components (the Android
# build only has X86), minus components without a library in the install.
# Targets the Android build really has (from its LLVMConfig.cmake).
targets() {
  sed -n 's/^set(LLVM_TARGETS_TO_BUILD \(.*\))$/\1/p' "$P/lib/cmake/llvm/LLVMConfig.cmake" | tr ';' ' '
}

components() {
  local c other skip
  local ours=" $(targets | tr 'A-Z' 'a-z') "
  mapfile -t others < <("$HOST" --targets-built | tr ' ' '\n' | tr 'A-Z' 'a-z' |
    while read -r t; do [[ $ours == *" $t "* ]] || echo "$t"; done)
  for c in $("$HOST" --components); do
    skip=0
    for other in "${others[@]}"; do [[ $c == "$other"* ]] && { skip=1; break; }; done
    ((skip)) && continue
    # Pseudo-components (all, engine, native, x86, ...) have no library.
    if compgen -G "$P/lib/libLLVM*.a" >/dev/null &&
       ! ls "$P"/lib/libLLVM*.a | sed 's#.*/libLLVM##; s#\.a$##' | tr 'A-Z' 'a-z' | grep -qx "$c"; then
      case $c in all|all-targets|engine|native|nativecodegen|x86|amdgpu) ;; *) continue ;; esac
    fi
    printf '%s ' "$c"
  done
  echo
}

# --system-libs (often combined with --libs --ldflags in ONE call, as meson
# does): the Android build needs none, so drop it and answer the rest.
args=()
for a in "$@"; do [[ $a == --system-libs ]] || args+=("$a"); done
(( ${#args[@]} )) || { echo; exit 0; }
set -- "${args[@]}"

for a in "$@"; do
  case $a in
    --components) components; exit 0 ;;
    --targets-built) targets; exit 0 ;;
    --host-target) echo x86_64-unknown-linux-android; exit 0 ;;
    --shared-mode) echo static; exit 0 ;;
    --link-shared) echo "llvm-config-android: only static LLVM is built" >&2; exit 1 ;;
  esac
done
# Pseudo-components (all, native, ...) expand to the host's targets and
# extras (Polly) on --libs/--libfiles: drop libraries the Android build lacks.
"$HOST" "$@" | sed "s#$host_prefix#$P#g" | tr ' ' '\n' | while read -r w; do
  case $w in
    # LLVM's own libraries (incl. host-only extras like Polly) must exist here.
    -l*) [[ -f $P/lib/lib${w#-l}.a || ! -f $host_prefix/lib/lib${w#-l}.a ]] || continue ;;
    $P/lib/*.a) [[ -f $w ]] || continue ;;
  esac
  printf '%s ' "$w"
done | sed 's/ $//'
echo
