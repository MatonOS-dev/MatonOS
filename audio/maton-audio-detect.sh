#!/system/bin/sh
# Enumerate ALSA endpoints and prepare an optional primary-policy override.
# Detection is asynchronous; the stock HAL always has a valid static ODM file
# and never waits for policy_ready.

log -t MatonAudio "scanning /proc/asound for primary endpoints"
output_choice=$(getprop persist.vendor.maton.audio.output)
input_choice=$(getprop persist.vendor.maton.audio.input)
best_output=
best_input=
best_output_rank=99
best_input_rank=99

for card_path in /proc/asound/card[0-9]*; do
    [ -d "$card_path" ] || continue
    card=${card_path##*card}
    card_id=$(cat "$card_path/id" 2>/dev/null)
    card_label=$(sed -n "/^ *$card /p" /proc/asound/cards 2>/dev/null)
    desc=$(echo "$card_id $card_label" | tr '[:upper:]' '[:lower:]')
    case "$desc" in
        *hdmi*|*displayport*|*dp*) rank=20 ;;
        *usb*) rank=0 ;;
        *hda*|*analog*|*pch*|*sof*) rank=1 ;;
        *) rank=10 ;;
    esac
    for pcm_path in "$card_path"/pcm[0-9]*; do
        [ -d "$pcm_path" ] || continue
        pcm=${pcm_path##*pcm}
        dev=${pcm%[pc]}
        direction=${pcm#"$dev"}
        address="CARD_${card}_DEV_${dev}"
        case "$direction" in
            p)
                if [ "$address" = "$output_choice" ]; then
                    best_output=$address
                    best_output_rank=-1
                elif [ -z "$best_output" ] || [ "$rank" -lt "$best_output_rank" ]; then
                    best_output=$address
                    best_output_rank=$rank
                fi
                ;;
            c)
                if [ "$address" = "$input_choice" ]; then
                    best_input=$address
                    best_input_rank=-1
                elif [ -z "$best_input" ] || [ "$rank" -lt "$best_input_rank" ]; then
                    best_input=$address
                    best_input_rank=$rank
                fi
                ;;
        esac
    done
done

[ -n "$best_output" ] && setprop vendor.maton.audio.no_output 0 || setprop vendor.maton.audio.no_output 1
[ -n "$best_input" ] && setprop vendor.maton.audio.no_input 0 || setprop vendor.maton.audio.no_input 1
[ -n "$best_output" ] || best_output=CARD_0_DEV_0
[ -n "$best_input" ] || best_input=CARD_0_DEV_0
setprop vendor.maton.audio.detected_output "$best_output"
setprop vendor.maton.audio.detected_input "$best_input"
tmp=/data/vendor/maton-audio/primary_audio_policy_configuration.xml.tmp
if cp /odm/etc/primary_audio_policy_configuration.xml "$tmp" &&
   sed -i \
       -e "/tagName=\"Speaker\"/ s/address=\"[^\"]*\"/address=\"$best_output\"/" \
       -e "/tagName=\"Built-In Mic\"/ s/address=\"[^\"]*\"/address=\"$best_input\"/" \
       "$tmp" &&
   mv "$tmp" /data/vendor/maton-audio/primary_audio_policy_configuration.xml; then
    setprop vendor.maton.audio.policy_ready 1
    log -t MatonAudio "selected output=$best_output input=$best_input; generated stock policy override"
else
    rm -f "$tmp"
    log -t MatonAudio "could not generate policy override; stock ODM card-0 policy remains"
fi
