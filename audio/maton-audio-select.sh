#!/system/bin/sh
# Select the first non-virtual ALSA card with an actual playback PCM. Keep
# discovery short: init runs this asynchronously with a three-second limit.

set_playback_mixer_max() {
    card=$1
    tinymix_bin=/system/bin/tinymix
    [ -x "$tinymix_bin" ] || return 0

    # tinymix's tab mode preserves the complete control name as one field.
    # Ignore individual failures: unusual mixer controls must never hold boot.
    tab=$(printf '\t')
    "$tinymix_bin" -D "$card" -a -t 2>/dev/null |
    while IFS="$tab" read -r index type count name values; do
        case "$name" in
            *Playback*) ;;
            *) continue ;;
        esac
        type=$(printf '%s' "$type" | tr '[:upper:]' '[:lower:]')
        case "$name:$type" in
            *Volume*:int)
                max=${values##*->}
                max=${max%%)*}
                case "$max" in ''|*[!0-9-]*) continue ;; esac
                # Start conservatively below full hardware gain. Android's
                # stream volume remains the user-facing control.
                default=$((max * 2 / 3))
                [ "$default" -gt 0 ] || default=1
                "$tinymix_bin" -D "$card" "$name" "$default" >/dev/null 2>&1 || :
                ;;
            *Switch*:bool)
                "$tinymix_bin" -D "$card" "$name" 1 >/dev/null 2>&1 || :
                ;;
        esac
    done
}

for cards_file in /proc/asound/cards; do
    [ -r "$cards_file" ] || break
    while IFS= read -r card_line; do
        # The cards file has one numbered [id] line per card, followed by an
        # indented description line. Word splitting is intentional here.
        set -- $card_line
        card_num=${1:-}
        card_id=${card_line#*[}
        card_id=${card_id%%]*}
        case "$card_num" in
            ''|*[!0-9]*) continue ;;
        esac
        case "$card_id" in
            *[Ll]oopback*|*[Dd]ummy*|*snd-aloop*) continue ;;
        esac

        while IFS= read -r pcm_line; do
            pcm_card=${pcm_line%%-*}
            pcm_rest=${pcm_line#*-}
            pcm_device=${pcm_rest%%:*}
            # /proc/asound/pcm prints card and device as two-digit numbers.
            while [ "${#pcm_card}" -gt 1 ] && [ "${pcm_card#0}" != "$pcm_card" ]; do
                pcm_card=${pcm_card#0}
            done
            while [ "${#pcm_device}" -gt 1 ] && [ "${pcm_device#0}" != "$pcm_device" ]; do
                pcm_device=${pcm_device#0}
            done
            [ "$pcm_card" = "$card_num" ] || continue
            case "$pcm_line" in *playback*) ;; *) continue ;; esac
            pcm_tail=${pcm_line#*playback}
            set -- $pcm_tail
            playback_count=${1:-0}
            case "$playback_count" in ''|*[!0-9]*) continue ;; esac
            [ "$playback_count" -gt 0 ] || continue

            setprop vendor.maton.audio.card "$card_num"
            setprop vendor.maton.audio.device "$pcm_device"
            # BayLibre caches its primary card on first stream; publish the
            # persistent selector inputs and completion before slow mixer IO.
            setprop persist.vendor.audio.primary.card "$card_num"
            setprop persist.vendor.audio.primary.device "$pcm_device"
            setprop vendor.maton.audio.selector_result found
            setprop vendor.maton.audio.selector_done 1
            set_playback_mixer_max "$card_num"
            exit 0
        done < /proc/asound/pcm
    done < "$cards_file"
done

setprop persist.vendor.audio.primary.card ''
setprop persist.vendor.audio.primary.device ''
setprop vendor.maton.audio.selector_result no_card
setprop vendor.maton.audio.selector_done 1
exit 0
