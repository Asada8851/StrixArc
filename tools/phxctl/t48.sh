#!/bin/bash
SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

ioreg -l | grep 'phx-build"' | head -1
ADOPT=1 FD=1 MC=1 CS=1 bash ~/gpu/full.sh kiqsched 2>&1 | tail -1
eval $SUDO_CMD bash -c "nohup shutdown -r +5 >/dev/null 2>&1 &"
cd ~/gpu/vkprobe && (VK_ICD_FILENAMES=$HOME/gpu/mesa/build/src/amd/vulkan/radeon_devenv_icd.x86_64.json nohup perl -e 'alarm 60; exec @ARGV' ./vkprobe > ~/gpu/vk.log 2>&1 &)
sleep 40
P(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl "$@" 2>&1; }
echo "--- vkprobe log"; tail -20 ~/gpu/vk.log
echo "--- gpu state"; P regs 0x2828 0x2829 0x282a | grep 0x | tr "\n" " "; echo
P wreg 0xA900 4 >/dev/null; P regs 0x3213 0x323f 0x320b | grep 0x | tr "\n" " "; P wreg 0xA900 0 >/dev/null; echo
P vram 0x2550000 2 | grep vram
P vram 0x2541010 2 | grep vram
echo "--- ring (dw 8..)"; P vram 0x2540020 24 | grep vram
eval $SUDO_CMD log show --last 3m --predicate 'eventMessage CONTAINS "PhxIB" OR eventMessage CONTAINS "adopted"' 2>/dev/null | grep -v "^Timestamp" | cut -c60-240 | grep -E 'PhxIB|adopted' | sed 's/.*kernel: //' | tail -40
eval $SUDO_CMD killall shutdown 2>/dev/null; echo done-t48
