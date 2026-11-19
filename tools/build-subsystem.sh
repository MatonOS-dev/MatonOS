#!/usr/bin/env bash
# Build the Linux subsystem (MatonOS-dev/linux-subsystem-app) with its own
# build.sh and stage the signed bootstrap APK the image imports from
# prebuilt/linux-subsystem/ (MatonLinuxSubsystem in the root Android.bp).
# The APK is not committed; SOURCE records what it was built from.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
REPO=${MATON_SUBSYSTEM_REPO:-$HOME/matonos/repos/linux-subsystem-app}
OUT=${MATON_SUBSYSTEM_OUT:-$HOME/matonos/build/main-out}
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
DEST=$DEVICE_DIR/prebuilt/linux-subsystem

[[ -x $REPO/build.sh ]] || die "subsystem checkout missing: $REPO (set MATON_SUBSYSTEM_REPO)"
for f in "$KEY_DIR/matonos-linux-subsystem.jks" "$KEY_DIR/matonos-linux-subsystem.pass"; do
  [[ -s $f ]] || die "missing subsystem signing input: $f"
done

head=$(git -C "$REPO" rev-parse HEAD)
info "Linux subsystem ($REPO @ ${head:0:7})"
# Skip the build (and its test suite) when the staged APK is from this exact,
# clean commit; MATON_SUBSYSTEM_FORCE=1 rebuilds anyway.
if [[ -z ${MATON_SUBSYSTEM_FORCE:-} && -s $DEST/SOURCE && -s $DEST/MatonLinuxSubsystemBootstrap.apk ]] &&
   [[ -z $(git -C "$REPO" status --porcelain) ]] &&
   grep -qx "commit: $head" "$DEST/SOURCE" &&
   grep -qF "$(cd "$DEST" && sha256sum MatonLinuxSubsystemBootstrap.apk)" "$DEST/SOURCE"; then
  info "Skipping: staged APK already built from clean $head"
  exit 0
fi
MATON_SUBSYSTEM_OUT=$OUT \
  MATON_SIGNING_STORE_FILE=$KEY_DIR/matonos-linux-subsystem.jks \
  MATON_SIGNING_PASSWORD_FILE=$KEY_DIR/matonos-linux-subsystem.pass \
  MATON_BUILD_JOBS=${MATON_BUILD_JOBS:-$(nproc)} \
  bash -c 'cd "$1" && ./build.sh' _ "$REPO"  # its tests use repo-relative paths

for f in MatonLinuxSubsystemBootstrap.apk MatonLinuxSubsystem.apk; do
  [[ -s $OUT/$f ]] || die "subsystem build produced no $f"
done
mkdir -p "$DEST"
install -m 0644 "$OUT/MatonLinuxSubsystemBootstrap.apk" "$DEST/MatonLinuxSubsystemBootstrap.apk"

flatpak_commit=$(awk -F'|' '$1=="flatpak"{print $4}' "$REPO/linux/flatpak/build-support/sources.lock")
version=$("$SDK/build-tools/36.0.0/aapt2" dump badging "$OUT/MatonLinuxSubsystem.apk" | sed -n "s/.*versionCode='\([0-9]*\)'.*/\1/p;q")
cert=$("$SDK/build-tools/36.0.0/apksigner" verify --print-certs "$DEST/MatonLinuxSubsystemBootstrap.apk" | awk '/SHA-256/{print $NF; exit}')
cat > "$DEST/SOURCE" <<EOF
v3.5 signed subsystem bootstrap APK (versionCode 1; embeds the real APK, installs it over itself on first boot)
Built by tools/build-subsystem.sh; not committed.
repo: https://github.com/MatonOS-dev/linux-subsystem-app
commit: $head$(git -C "$REPO" diff --quiet HEAD || echo ' (dirty)')
flatpak-repo: https://github.com/MatonOS-dev/flatpak
flatpak-commit: $flatpak_commit
certificate-sha256: $cert

SHA-256:
$(cd "$DEST" && sha256sum MatonLinuxSubsystemBootstrap.apk)
$(cd "$OUT" && sha256sum MatonLinuxSubsystem.apk | sed "s/\$/ (embedded, versionCode='$version')/")
EOF
info "Staged $DEST/MatonLinuxSubsystemBootstrap.apk (versionCode $version)"
