#!/system/bin/sh
# matonos-setup.sh: MatonOS defaults Android has no config for, applied after
# boot (setup/matonos-setup.rc). Each step only acts while the setting is
# still untouched, so whatever the user changes later stays changed.

# True while an app-op has never been set (still in its default mode).
op_is_default() {
    case $(cmd appops get --user 0 "$1" "$2" 2>/dev/null) in
        *"$2: allow"*|*"$2: deny"*|*"$2: ignore"*|*"$2: errored"*) return 1 ;;
    esac
    return 0
}

# F-Droid may install apps ("Install unknown apps" allowed).
fdroid=org.fdroid.fdroid
if op_is_default "$fdroid" REQUEST_INSTALL_PACKAGES; then
    cmd appops set --user 0 "$fdroid" REQUEST_INSTALL_PACKAGES allow
fi

# Test audio clips (media/test-audio in the device tree, shipped in
# /product/media/matonos/test-audio) go into Music once, through MediaProvider
# so they're owned and indexed correctly. Deleting them later sticks.
media_uri=content://media/external_primary/audio/media
if [ "$(settings get global matonos_test_audio_installed)" != 1 ]; then
    for f in /product/media/matonos/test-audio/*.mp3; do
        [ -f "$f" ] || continue
        name=${f##*/}
        content insert --uri "$media_uri" --bind _display_name:s:"$name" \
            --bind relative_path:s:Music/ --bind mime_type:s:audio/mpeg
        id=$(content query --uri "$media_uri" --projection _id \
            --where "_display_name='$name' AND relative_path='Music/'" |
            sed -n 's/.*_id=\([0-9]*\).*/\1/p' | tail -n 1)
        [ -n "$id" ] && content write --uri "$media_uri/$id" < "$f"
    done
    settings put global matonos_test_audio_installed 1
fi

exit 0
