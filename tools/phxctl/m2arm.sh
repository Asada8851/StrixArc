#!/bin/bash
# M2: bring the GPU up, let PhxAccel + PhxFB start, register the panel with the accelerator and open the display pipe.
S="sudo"
if [ -n "$SUDO_PASS" ]; then
    S="echo '$SUDO_PASS' | sudo -S -p ''"
fi
bash ~/gpu/phxup.sh
sleep 2
eval $S dmesg | grep -E "PhxFB|watching for PhxFB|published -- display machine|NMXAccel: start" | tail -6
eval $S sysctl -w debug.nvaccelfb=1 >/dev/null; sleep 1
eval $S dmesg | grep -E "registerFramebuffer|found_framebuffer|count after|restored|GATES" | tail -8
eval $S sysctl -w debug.nvaccel_iop=1 >/dev/null; eval $S sysctl -w debug.nvaccelfb=3 >/dev/null; sleep 1
eval $S dmesg | grep -E "level 3|scanout-backed|delivering|GATES" | tail -4
ioreg -rc NMXAccel -d1 | grep -c MetalPluginName | sed 's/^/plugin name on accelerator: /'
sysctl debug.nvaccelfb debug.nvaccel_iop debug.nmxaccel_headless 2>&1
cd ~/gpu && perl -e "alarm 60; exec @ARGV" ./mtlprobe 2>&1 | tail -1
