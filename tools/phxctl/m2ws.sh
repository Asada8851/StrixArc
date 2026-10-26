#!/bin/bash
# M2: put WindowServer on the 780M once (after m2arm.sh), with a fall-back: if WindowServer crashes 3 times within the
# watch window, take it off the allow list and restart it on the software path. Usage (root): m2ws.sh [watch seconds]
set -u
WATCH=${1:-90}
ALLOW=/Library/GPUBundles/nvmtl-allow.txt
DR=/Library/Logs/DiagnosticReports
sysctl -w debug.phxagdc=1 >/dev/null; sleep 1
echo "agdc state $(sysctl -n debug.phxagdc)"
dmesg | grep PhxAGDC | tail -3
printf '# PhxAccel M2: WindowServer composites on the 780M once armed\n!WindowServer\n' > $ALLOW
before=$(ls $DR | grep -c '^WindowServer-')
pkill -9 -x WindowServer
for ((t = 0; t < WATCH; t += 5)); do
    sleep 5
    n=$(( $(ls $DR | grep -c '^WindowServer-') - before ))
    if [ $n -ge 3 ]; then
        printf '# PhxAccel M2 paused: WindowServer crashed on the 780M\n' > $ALLOW
        sysctl -w debug.phxagdc=0 >/dev/null   # with the AGDC up, WindowServer aborted on the software path too
        pkill -9 -x WindowServer
        echo "FALLBACK after $t s: $n WindowServer crashes, back on the software path"
        exit 1
    fi
done
echo "WindowServer pid $(pgrep -x WindowServer), crashes in $WATCH s: $n"
