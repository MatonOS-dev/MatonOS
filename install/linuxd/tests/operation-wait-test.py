#!/usr/bin/env python3
"""Run the actual shell wait with simulated operation snapshots and time."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
script=root/'install/tools/test-flatpak.sh'
text=script.read_text()
wait=text[text.index('wait_for_ref()'):text.index('"$ADB" connect')]
# Preserve production helper resolution without requiring adb or a device.
wait=wait.replace('$(dirname "$0")',str(root/'install/tools'))
with tempfile.TemporaryDirectory() as tmp:
    for mode in ('slow','stalled','error','old-ref','handoff'):
        harness=Path(tmp)/'test.sh'
        harness.write_text('''set -Eeuo pipefail
IDLE_MINUTES=2
SECONDS=0
step=0
sleep() { step=$((step+1)); SECONDS=$((SECONDS+61)); }
adb_cmd() {
    local seq=$step phase=staging error=
    if [[ $MODE == stalled ]]; then seq=0; fi
    if [[ $MODE == error ]]; then error=',"error":"Nothing matches"'; fi
    if [[ $MODE == handoff ]] && ((step>=10)); then phase=handoff; fi
    if ((step>=15)); then phase=complete; fi
    printf '{"op":{"phase":"%s","sequence":%s%s}}\\n' "$phase" "$seq" "$error"
}
list_installed() {
    if ((step>=15)) || [[ $MODE == old-ref ]]; then
        echo '{"output":"org.example.App/x86_64/stable"}'
    else echo '{"output":""}'; fi
}
'''+wait+'''
if wait_for_ref app/org.example.App/x86_64/stable yes op; then rc=0; else rc=$?; fi
case "$MODE" in
slow|old-ref|handoff) [[ $rc == 0 && $step == 15 ]];;
stalled) [[ $rc != 0 && $step == 2 ]];;
error) [[ $rc != 0 && $step == 0 ]];;
esac
''')
        subprocess.run(['bash',str(harness)],env={'MODE':mode,'PATH':'/usr/bin:/bin'},check=True)
        print('PASS operation wait',mode)

# Exercise the production normalizer: speed/ETA redraws are not progress.
source=(root/'install/linuxd/MatonosLinuxd.cpp').read_text()
body=source[source.index('thread_local std::string g_last_staging_progress;'):source.index('class ListenerDeathRecipient')]
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'progress.cpp'
    path.write_text('#include <string>\n#include <cassert>\nstatic unsigned count;\nstd::string g_staging_operation="op";\nvoid Snapshot(const std::string&,const char*){count++;}\n'+body+
        'int main(){StagingProgress("Installing 10% 1 MB/s");StagingProgress("Installing 10% 2 MB/s");assert(count==1);StagingProgress("Installing 11% 2 MB/s");assert(count==2);}')
    binary=Path(tmp)/'progress'
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    print('PASS production staging progress ignores speed-only redraws')
