#!/system/bin/sh
# Noncritical boot task: try loading the stable ALSA card, then request the
# stock HAL's paced stub if module insertion failed. This runs asynchronously
# and is never a prerequisite for starting the HAL or completing boot.
/vendor/bin/modprobe -d /vendor/lib/modules -d /odm/lib/modules snd-aloop
if [ -d /sys/module/snd_aloop ]; then
    setprop vendor.maton.audio.loopback_ready 1
else
    setprop vendor.maton.audio.loopback_ready 0
fi
exit 0
