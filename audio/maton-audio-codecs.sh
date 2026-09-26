#!/system/bin/sh
# Ensure HDA codec bus drivers are registered even if the controller's PCI
# modalias caused snd_hda_intel to load first. modprobe follows modules.dep for
# each codec's dependencies. Keep failures non-fatal and reload the controller
# after registering codecs so it enumerates the codec(s) again.

MODPROBE=/system/bin/modprobe
MODULE_DIR=/vendor/lib/modules

[ -x "$MODPROBE" ] || exit 0
[ -r "$MODULE_DIR/modules.dep" ] || exit 0

for module in "$MODULE_DIR"/snd-hda-codec-*.ko; do
    [ -f "$module" ] || continue
    name=${module##*/}
    name=${name%.ko}
    "$MODPROBE" -d "$MODULE_DIR" "$name" >/dev/null 2>&1 || :
done

# No audio client is expected to have opened the card this early. If removal
# fails, leave the existing driver alone; loading it again remains harmless.
"$MODPROBE" -d "$MODULE_DIR" -r snd_hda_intel >/dev/null 2>&1 || :
"$MODPROBE" -d "$MODULE_DIR" snd_hda_intel >/dev/null 2>&1 || :
exit 0
