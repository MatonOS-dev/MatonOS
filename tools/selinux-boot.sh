#!/usr/bin/env bash
# Shared boot-profile guard. Source before packaging or installing a payload.
maton_selinux_init() {
  case ${MATON_SELINUX_PERMISSIVE:-0} in
    0) MATON_SELINUX_MODE=enforcing ;;
    1) MATON_SELINUX_MODE=permissive ;;
    *) echo "ERROR: MATON_SELINUX_PERMISSIVE must be 0 or 1" >&2; return 1 ;;
  esac
  case ${MATON_RELEASE:-0} in
    0|1) ;;
    *) echo "ERROR: MATON_RELEASE must be 0 or 1" >&2; return 1 ;;
  esac
  if [[ $MATON_SELINUX_MODE == permissive &&
        ( ${MATON_RELEASE:-0} == 1 || ${MATON_REQUIRE_2023_SHIM:-0} == 1 ||
          ${TARGET_BUILD_VARIANT:-} == user ) ]]; then
    echo "ERROR: release packaging refuses MATON_SELINUX_PERMISSIVE=1" >&2
    return 1
  fi
}

maton_selinux_validate() {
  local cmdline=$1 word mode_count=0
  local -a words
  # Keep parsing identical for the kernel and payload.conf scalar reader.
  if [[ $cmdline == *[\"\'\\]* || $cmdline == *$'\n'* || $cmdline == *$'\r'* ]]; then
    echo "ERROR: boot command lines must not contain quotes, backslashes or newlines" >&2
    return 1
  fi
  read -r -a words <<< "$cmdline"
  for word in "${words[@]}"; do
    if [[ $word == androidboot.selinux=* ]]; then
      mode_count=$((mode_count + 1))
      if (( mode_count > 1 )); then
        echo "ERROR: duplicate androidboot.selinux boot options" >&2
        return 1
      fi
    fi
    case $word in
      androidboot.selinux=enforcing|enforcing=1|selinux=1) ;;
      androidboot.selinux=permissive)
        [[ $MATON_SELINUX_MODE == permissive ]] && continue
        echo "ERROR: permissive boot requires MATON_SELINUX_PERMISSIVE=1" >&2
        return 1 ;;
      --|androidboot.selinux*|androidboot.enforcing*|enforcing*|selinux*|security=*|lsm=*)
        echo "ERROR: unsupported SELinux/LSM boot override: $word" >&2
        return 1 ;;
    esac
  done
}

maton_selinux_cmdline() {
  maton_selinux_validate "$1" || return 1
  if [[ " $1 " =~ [[:space:]]androidboot\.selinux= ]]; then
    printf '%s\n' "$1"
  else
    printf '%s androidboot.selinux=%s\n' "$1" "$MATON_SELINUX_MODE"
  fi
}
