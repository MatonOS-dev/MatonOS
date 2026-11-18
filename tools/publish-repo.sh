#!/usr/bin/env bash
# publish-repo.sh: sign and publish the MatonOS F-Droid repo (tar over ssh).
#
# Usage: ./publish-repo.sh [-n]        (-n: sign locally, don't upload)
#
# The repo lives on this PC (MATON_REPO_DIR, default ~/matonos-repo; its
# keystore in ~/.config/matonos/fdroid, never uploaded) and is served by the
# MatonOS OTA server (github.com/MatonOS-dev/matonos-ota) under /fdroid/ at
# MATON_REPO_URL. It sits behind Cloudflare's edge cache; the server gives
# index files a 60 s cache and everything else (versioned names) an immutable one.
#   - APKs, icons and index diffs (diff/, immutable) are uploaded before the index that references them, so a
#     client never sees an index pointing at a file that isn't there yet.
# fdroidserver comes from ~/.local/opt/fdroidserver (pip --target): the
# distro's 2.2.1 can't parse resources of APKs targeting SDK 36+.
set -Eeuo pipefail
die() { echo "publish-repo: $*" >&2; exit 1; }
info() { echo "==> $*"; }

HERE=$(dirname "$(readlink -f "$0")")
AOSP=$(readlink -f "$HERE/../../../..")
REPO=${MATON_REPO_DIR:-$HOME/matonos-repo}
FDROID_LIB=${MATON_FDROID_LIB:-$HOME/.local/opt/fdroidserver}
REMOTE=${MATON_REPO_REMOTE:?set MATON_REPO_REMOTE (host:dir of the OTA server F-Droid dir) in matonos.local.env}
URL=${MATON_REPO_URL:?set MATON_REPO_URL (public repo URL) in matonos.local.env}
UPLOAD=1
while getopts "nh" opt; do
  case $opt in
    n) UPLOAD=0 ;;
    *) sed -n '2,16p' "$0"; exit 0 ;;
  esac
done

[[ -f $REPO/config.yml ]] || die "no repo at $REPO (run fdroid init there first)"
[[ -d $FDROID_LIB/fdroidserver ]] || die "fdroidserver missing in $FDROID_LIB"

info "Signing index"
rm -f "$REPO"/repo/*.hwfm   # old download-site sidecars
(cd "$REPO" && PATH="$AOSP/prebuilts/jdk/jdk21/linux-x86/bin:$PATH" PYTHONPATH="$FDROID_LIB" \
  python3 -c 'import fdroidserver.__main__ as m; m.main()' update)

INDEX_FILES=(entry.jar entry.json index-v2.json index-v1.jar index-v1.json index.jar index.xml index.html)

(( UPLOAD )) || { info "Signed locally only (-n)"; exit 0; }

# The server has no rsync: stream files with tar over ssh (APKs/icons, then index).
host=${REMOTE%%:*}; dir=${REMOTE#*:}
[[ $host =~ ^[A-Za-z0-9._@-]+$ && $dir =~ ^[A-Za-z0-9._/-]+$ && $dir != *..* ]] || die "unsafe remote host or directory in MATON_REPO_REMOTE"
push() {
  local remote_dir
  printf -v remote_dir '%q' "$dir/repo"
  tar -C "$REPO/repo" -cf - "$@" | ssh "$host" "mkdir -p -- $remote_dir && tar -C $remote_dir -xf -"
}
mapfile -t payload < <(cd "$REPO/repo" && find . -mindepth 1 -maxdepth 1 \( -name '*.apk' -o -name '*.zip' -o -name 'icons*' -o -name 'diff' -o -name 'index.css' -o -name 'index.png' -o -name 'status' \) -printf '%P\n')
info "Uploading APKs and icons to $REMOTE"
push "${payload[@]}"
mapfile -t index < <(cd "$REPO/repo" && for f in "${INDEX_FILES[@]}"; do [[ -f $f ]] && printf '%s\n' "$f"; done)
info "Uploading index"
push "${index[@]}"
info "Published: $URL"
