#!/usr/bin/env bash
# Device smoke test: install a Flathub app through the MatonOS store UI on the disk VM (adb localhost:5556),
# launch its stub, collect errors/denials and a screenshot. Usage: tools/smoke-flatpak-app.sh "<search>" "<store title>"
# smoke.sh "<search text>" "<exact store title>" — install via the store, launch, report.
set -u
S=localhost:5556; a(){ adb -s $S shell "$@"; }
q=$1; title=$2; out=${MATON_SMOKE_OUT:-/mnt/data/aosp/out/pc-logs/smoke}/$(echo "$title" | tr ' /' '__')
mkdir -p $out
dump(){ a uiautomator dump /data/local/tmp/u.xml >/dev/null 2>&1; a cat /data/local/tmp/u.xml; }
a am force-stop org.matonos.flathub; a am start -n org.matonos.flathub/.MainActivity >/dev/null; sleep 10
eb=$(dump | grep -oE '<node[^>]*EditText[^>]*>' | grep -oE 'bounds="[^"]+"' | head -1 | grep -oE '[0-9]+' | tr '\n' ' '); read ex1 ey1 ex2 ey2 <<<"$eb"; a input tap $(((ex1+ex2)/2)) $(((ey1+ey2)/2)); sleep 1; a input text "$(echo "$q" | sed 's/ /%s/g')"; a input keyevent 66; sleep 8
x=$(dump)
# The Install button on the same row as the title: the first "Install" whose top is within 40px of the title's top.
ty=$(echo "$x" | grep -oE "text=\"$title\"[^>]*bounds=\"\[[0-9]+,[0-9]+\]" | head -1 | grep -oE '\[[0-9]+,[0-9]+\]$' | tr -d '[]' | cut -d, -f2)
[ -z "$ty" ] && { echo "NOT FOUND: $title"; echo "$x" | grep -oE 'text="[^"]+"' | sed -n 8,20p; exit 2; }
b=$(echo "$x" | grep -oE 'text="Install"[^>]*bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' | while read -r l; do y=$(echo "$l" | grep -oE '\[[0-9]+,[0-9]+\]' | head -1 | tr -d '[]' | cut -d, -f2); d=$((y-ty)); [ ${d#-} -le 40 ] && { echo "$l"; break; }; done)
[ -z "$b" ] && { echo "NO INSTALL BUTTON for $title (already installed?)"; exit 3; }
read x1 y1 x2 y2 <<<"$(echo "$b" | grep -oE '[0-9]+' | tail -4 | tr '\n' ' ')"
adb -s $S install -r $HOME/matonos/build/main-out/MatonLinuxSubsystem.apk >/dev/null 2>&1; a logcat -c; t0=$(date +%s); a input tap $(((x1+x2)/2)) $(((y1+y2)/2)); sleep 15
for i in $(seq 180); do t=$(dump | grep -oE 'text="(download|Installing|Open|Failed|Error|Invalid|java)[^"]*"' | head -2 | tr '\n' ' '); case "$t" in *Installing*|*download*) sleep 10;; *) echo "install: $t after $(( $(date +%s)-t0 ))s"; break;; esac; done
a logcat -d > $out/install.log
P=$(a pm list packages | grep "^package:flatpak.app" | cut -d: -f2 | tr -d "\r" | grep -i "$(echo "$title" | tr -d " " | cut -c1-6)" | head -1)
echo "package: $P"
a logcat -c; a am start -n $P/.Launcher >/dev/null 2>&1; sleep 60
a dumpsys activity activities | grep -m1 topResumedActivity
a logcat -d > $out/launch.log
grep MatonV3 $out/launch.log | grep -vE "trap |raw-calls|handler-stack|path |v3 mount|bwrap argv|setenv|^--" | grep -iE "error|fatal|bwrap:|exited|Can't|warning" | head -8 | cut -c40-220
grep "avc:.*denied" $out/launch.log | grep -vE "uiautomator|tcontext=u:r:su|shell_test_data|gmain|pipefs|snd|udmabuf|sysfs" | sed -E 's/.*avc: +denied +\{ ([^}]+)\}.*comm="([^"]+)".*tcontext=([^ ]+) tclass=([^ ]+).*/\1 \2 \3 \4/' | sort | uniq -c | sort -rn | head -5
a screencap -p /data/local/tmp/s.png; adb -s $S pull /data/local/tmp/s.png $out/screen.png >/dev/null 2>&1; echo "screenshot: $out/screen.png"
