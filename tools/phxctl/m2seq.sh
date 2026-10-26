#!/bin/bash
# M2 sequence after a boot with PhxAMDFB (root, runs detached): bring-up + arming, WindowServer on the 780M twice
# (GPUWrangler learns the accelerator on the first), then what the compositor and the display pipe did. Log: /tmp/m2seq.log
exec > /tmp/m2seq.log 2>&1
shutdown -r +15 >/dev/null 2>&1 &
SUDO_PASS=${SUDO_PASS:-} bash /Users/Shared/gpu/m2arm.sh 2>&1 | grep -E "plugin name|results"
bash /Users/Shared/gpu/m2ws.sh 30 | tail -1
dmesg > /tmp/m2seq.dm0
echo "T0 $(date "+%Y-%m-%d %H:%M:%S")" > /tmp/m2t0
bash /Users/Shared/gpu/m2ws.sh 45 | tail -1
sample WindowServer 2 -file /tmp/ws.sample >/dev/null 2>&1
grep -E "Compositor(SW|Metal)::CompositeLayersToDestination" /tmp/ws.sample | sed -E 's/^[ +!:|]*//' | cut -c1-80 | sort | uniq -c | head -4
bash /Users/Shared/gpu/m2check.sh
dmesg | diff /tmp/m2seq.dm0 - | grep '^>' | grep -v -E 'Sandbox|process_is|PhxIB|AMFI' | awk -F']: ' '{print $2}' | cut -c1-200 | sort | uniq -c | sort -rn | head -40
pkill -f "shutdown -r"
echo DONE
