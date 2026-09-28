#!/usr/bin/env bash
# publish-repo.sh: sign and publish the MatonOS F-Droid repo (tar over ssh).
#
# Usage: ./publish-repo.sh [-n]        (-n: sign locally, don't upload)
#
# The repo lives on this PC (MATON_REPO_DIR, default ~/matonos-repo; its
# keystore in ~/.config/matonos/fdroid, never uploaded) and is served from the
# HWFM file server at https://download.hanro50.net.za/matonos/fdroid/repo.
# The site is behind Cloudflare's edge cache, so:
#   - index files get <file>.hwfm sidecars (HWFM header overrides) with a
#     60 s cache, APKs keep HWFM's one-day default (their names are versioned);
#   - APKs and icons are uploaded before the index that references them, so a
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
REMOTE=${MATON_REPO_REMOTE:-han-mc-server:server/Download/download/matonos/fdroid}
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
# fdroid update treats every file in repo/ as a package: sidecars come after.
rm -f "$REPO"/repo/*.hwfm
(cd "$REPO" && PATH="$AOSP/prebuilts/jdk/jdk21/linux-x86/bin:$PATH" PYTHONPATH="$FDROID_LIB" \
  python3 -c 'import fdroidserver.__main__ as m; m.main()' update)

INDEX_FILES=(entry.jar entry.json index-v2.json index-v1.jar index-v1.json index.jar index.xml index.html)
for f in "${INDEX_FILES[@]}"; do
  [[ -f $REPO/repo/$f ]] &&
    printf '{"Cache-Control": "public, max-age=60", "CDN-Cache-Control": "max-age=60"}\n' \
      > "$REPO/repo/$f.hwfm"
done

(( UPLOAD )) || { info "Signed locally only (-n)"; exit 0; }

# The server has no rsync: stream files with tar over ssh (APKs/icons, then index).
host=${REMOTE%%:*}; dir=${REMOTE#*:}
push() { tar -C "$REPO/repo" -cf - "$@" | ssh "$host" "mkdir -p '$dir/repo' && tar -C '$dir/repo' -xf -"; }
mapfile -t payload < <(cd "$REPO/repo" && find . -mindepth 1 -maxdepth 1 \( -name '*.apk' -o -name '*.zip' -o -name 'icons*' -o -name 'index.css' -o -name 'index.png' -o -name 'status' \) -printf '%P\n')
info "Uploading APKs and icons to $REMOTE"
push "${payload[@]}"
mapfile -t index < <(cd "$REPO/repo" && for f in "${INDEX_FILES[@]}"; do [[ -f $f ]] && printf '%s\n%s\n' "$f.hwfm" "$f"; done)
info "Uploading index"
push "${index[@]}"
info "Published: https://download.hanro50.net.za/matonos/fdroid/repo"
