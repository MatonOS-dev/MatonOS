#!/usr/bin/env bash
set -euo pipefail

WORKTREE=$(cd "$(dirname "$0")/../../.." && pwd)
CACHE=${MATONOS_FLATPAK_TEST_REPO:-/mnt/data/aosp/out/pc-logs/musl-324/layout-test-final3/R}
STATIC=${MATONOS_FLATPAK_TEST_BINARY:-$WORKTREE/linux/flatpak/prebuilt/static/x86_64/matonos-flatpak}
STORE=${MATONOS_FLATPAK_STORE_TEST_BINARY:-$WORKTREE/linux/flatpak/prebuilt/static/x86_64/matonos-flatpak-store}
LAUNCHER_PROBE=${MATONOS_FLATPAK_LAUNCHER_PROBE:-}
APP_REF=app/org.gnome.Calculator/x86_64/stable
APP_COMMIT=0032886e3396c2e0cb107d453d600301c8090241d3fef31ca3f688510dba0e43
RUNTIME_REF=runtime/org.gnome.Platform/x86_64/51
RUNTIME_COMMIT=95a671a9704f98cf4ec4904f24a3ddafe546262eac89c56ac7c59e51feac3bf5
REMOTE=flathub
HARNESS_START_NS=$(date +%s%N)

report_case_time() {
  local name=$1 started=$2 elapsed_ms
  elapsed_ms=$(( ($(date +%s%N) - started) / 1000000 ))
  echo "TIME $name ${elapsed_ms}ms"
}

if [[ ! -x "$STATIC" || ! -d "$CACHE" ]]; then
  echo "static Flatpak binary or cached Flathub repo is unavailable" >&2
  exit 2
fi
if [[ -x "$STORE" ]]; then
  "$STORE" selftest
  echo "PASS static-musl installer store helper selftest"
fi
TMP=$(mktemp -d "$WORKTREE/.flatpak-publish-test.XXXXXX")
cleanup() {
  if [[ ${MATONOS_PUBLISH_KEEP_TMP:-0} == 1 ]]; then
    echo "Kept Flatpak publish test directory: $TMP" >&2
  else
    rm -rf "$TMP"
  fi
}
trap cleanup EXIT
mkdir -p "$TMP/bin"
ln -s "$STATIC" "$TMP/bin/flatpak"
ln -s "$STATIC" "$TMP/bin/ostree"
cc -DMATONOS_PUBLISH_HOST_TEST -Wall -Wextra -Werror \
  "$WORKTREE/install/linuxd/FlatpakPublish.c" \
  "$WORKTREE/install/linuxd/tests/flatpak-publish-driver.c" \
  -o "$TMP/publish-driver"
export MATONOS_FLATPAK_CLI="$TMP/bin/flatpak"
export MATONOS_OSTREE_CLI="$TMP/bin/ostree"
export MATONOS_PUBLISH_SKIP_STAGE=1
export PATH="$TMP/bin:$PATH"
export HOME="$TMP/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_DATA_HOME="$HOME/.local/share"
export DBUS_SESSION_BUS_ADDRESS=unix:path="$TMP/no-session-bus"
export SSL_CERT_DIR=/etc/ssl/certs
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

run_flatpak() { "$TMP/bin/flatpak" "$@"; }
setup_case() {
  local name=$1 source_mode=${2:-link}
  local base="$TMP/$name"
  local source=$CACHE
  mkdir -p "$base/R" "$base/S" "$base/U"
  if [[ $source_mode == link ]]; then
    source="$base/source"
    mkdir -p "$source"
    cp -al "$CACHE/." "$source/"
  fi
  mkdir -p "$base/R/repo"
  cp -al "$source/." "$base/R/repo/"
  "$TMP/bin/ostree" --repo="$base/R/repo" --group="remote \"$REMOTE\"" config set url "file://$source"
  for install in S U; do
    local repo="$base/$install/repo"
    mkdir -p "$repo"
    "$TMP/bin/ostree" --repo="$repo" init --mode=bare-user-only
    cp "$source/flathub.trustedkeys.gpg" "$repo/"
    "$TMP/bin/ostree" --repo="$repo" --group="remote \"$REMOTE\"" config set url "file://$source"
    "$TMP/bin/ostree" --repo="$repo" --group="remote \"$REMOTE\"" config set gpg-verify true
    "$TMP/bin/ostree" --repo="$repo" --group="remote \"$REMOTE\"" config set gpg-verify-summary true
  done
  printf '%s\n%s\n' "$base" "$source"
}
assert_unpublished() {
  local base=$1
  local sys user
  sys=$(FLATPAK_SYSTEM_DIR="$base/S" run_flatpak list --system --columns=ref)
  user=$(FLATPAK_USER_DIR="$base/U" run_flatpak list --user --columns=ref)
  [[ $sys != *"${RUNTIME_REF#runtime/}"* && $user != *"${APP_REF#app/}"* ]]
}
assert_no_target_objects() {
  local base=$1 install found
  for install in S U; do
    found=$(find "$base/$install/repo/objects" -type f -print -quit)
    [[ -z $found ]] || {
      echo "FAIL objects landed in $install after rejected import: $found" >&2
      return 1
    }
  done
}
run_publish() {
  local base=$1 app_pin=$2
  "$TMP/publish-driver" "$APP_REF" "$app_pin" "$RUNTIME_REF" "$RUNTIME_COMMIT" \
    "$REMOTE" "$base/R" "$base/S" "$base/U"
}

# Good signed commits publish into the system runtime and per-user app installs.
case_start=$(date +%s%N)
readarray -t GOOD < <(setup_case good direct)
run_publish "${GOOD[0]}" "$APP_COMMIT"
FLATPAK_SYSTEM_DIR="${GOOD[0]}/S" run_flatpak list --system --columns=ref | grep -F "${RUNTIME_REF#runtime/}" >/dev/null
FLATPAK_SYSTEM_DIR="${GOOD[0]}/S" FLATPAK_USER_DIR="${GOOD[0]}/U" \
  run_flatpak list --user --columns=ref | grep -F "${APP_REF#app/}" >/dev/null
FLATPAK_SYSTEM_DIR="${GOOD[0]}/S" run_flatpak override --show --system |
  grep -F 'sockets=session-bus' >/dev/null
FLATPAK_SYSTEM_DIR="${GOOD[0]}/S" FLATPAK_USER_DIR="${GOOD[0]}/U" \
  run_flatpak override --show --user | grep -F 'sockets=session-bus' >/dev/null
[[ -f ${GOOD[0]}/S/overrides/global && -f ${GOOD[0]}/U/overrides/global ]]
[[ ! -e ${GOOD[0]}/S/overrides/org.gnome.Calculator &&
   ! -e ${GOOD[0]}/U/overrides/org.gnome.Calculator ]]
echo "PASS good signed runtime/app published"
if [[ ${MATONOS_PUBLISH_RUN_LAUNCH_TEST:-0} == 1 ]]; then
  export MATONOS_FLATPAK_LAUNCHER_PROBE="$LAUNCHER_PROBE"
  bash "$WORKTREE/linux/flatpak/tests/launch-chain-host-probe.sh" "${GOOD[0]}/S" "${GOOD[0]}/U"
fi
report_case_time good "$case_start"

# A wrong pin is rejected before pull-local or either Flatpak deployment.
case_start=$(date +%s%N)
readarray -t WRONG < <(setup_case wrong direct)
if run_publish "${WRONG[0]}" "$(printf '0%.0s' {1..64})"; then
  echo "FAIL wrong pinned app commit was accepted" >&2; exit 1
fi
assert_unpublished "${WRONG[0]}"
echo "PASS wrong pinned commit leaves S/U unpublished"
report_case_time wrong-pin "$case_start"

# Corrupt the staged app commit object while retaining the signed summary.
case_start=$(date +%s%N)
readarray -t TAMPER < <(setup_case tamper link)
obj="${TAMPER[0]}/R/repo/objects/00/32886e3396c2e0cb107d453d600301c8090241d3fef31ca3f688510dba0e43.commitmeta"
[[ -f $obj ]] || { echo "cached app commit object not found: $obj" >&2; exit 2; }
cp "$obj" "$obj.bad"
python3 - "$obj.bad" <<'PY'
import sys
p = sys.argv[1]
with open(p, "r+b") as f:
    b = f.read(1)
    f.seek(0)
    f.write(bytes([b[0] ^ 0x01]))
PY
mv "$obj.bad" "$obj"
if run_publish "${TAMPER[0]}" "$APP_COMMIT"; then
  echo "FAIL tampered commit object was accepted" >&2; exit 1
fi
assert_unpublished "${TAMPER[0]}"
echo "PASS tampered commit aborts with S/U unpublished"
report_case_time tampered-metadata "$case_start"

# Corrupt an app payload object in R without touching the metadata commit or
# the cached source repo. pull-local --untrusted must reject the checksum.
case_start=$(date +%s%N)
readarray -t CORRUPT < <(setup_case corrupt link)
content_sum=$("$TMP/bin/ostree" --repo="${CORRUPT[0]}/R/repo" ls -R -C "$APP_COMMIT" |
  awk '$NF == "/files/bin/gnome-calculator" { print $5; exit }')
[[ $content_sum =~ ^[0-9a-f]{64}$ ]] || {
  echo "could not locate Calculator CONTENT object" >&2; exit 2;
}
obj="${CORRUPT[0]}/R/repo/objects/${content_sum:0:2}/${content_sum:2}.file"
[[ -f $obj ]] || { echo "Calculator CONTENT object not found: $obj" >&2; exit 2; }
cp "$obj" "$obj.bad"
python3 - "$obj.bad" <<'PY'
import sys
p = sys.argv[1]
with open(p, "r+b") as f:
    b = f.read(1)
    f.seek(0)
    f.write(bytes([b[0] ^ 0x01]))
PY
mv "$obj.bad" "$obj"
if run_publish "${CORRUPT[0]}" "$APP_COMMIT"; then
  echo "FAIL corrupted CONTENT object was accepted" >&2; exit 1
fi
assert_unpublished "${CORRUPT[0]}"
assert_no_target_objects "${CORRUPT[0]}"
echo "PASS corrupted CONTENT object aborts with S/U unpublished"
report_case_time corrupt-content "$case_start"
total_ms=$(( ($(date +%s%N) - HARNESS_START_NS) / 1000000 ))
echo "TIME full-harness ${total_ms}ms"
