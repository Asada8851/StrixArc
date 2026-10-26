#!/bin/bash

SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

# Full GPU bring-up (compute + gfx queues adopted by PhxBringup), then the Metal accelerator claim (PhxAccel starts).
# full.sh arms a 4-minute reboot watchdog and disarms it at its end.
MC=1 CS=1 ADOPT=1 GFX=1 AG=1 SUDO_PASS=${SUDO_PASS:-} bash ~/gpu/full.sh kiqsched 2>&1 | grep -E "^compute:|^gfx:|adopt" 
eval "$SUDO_CMD" ~/gpu/phxctl/mesboot claim
sleep 3
ioreg -rc NMXAccel -d1 | grep -c MetalPluginName | sed 's/^/NMXAccel with plugin name: /'
