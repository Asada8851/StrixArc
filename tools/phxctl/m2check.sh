#!/bin/bash
# M2 status after m2ws.sh (root): what IOPresentment and the display pipe made of the panel.
T0=$(cut -d" " -f2- /tmp/m2t0 2>/dev/null)
echo "agdc $(sysctl -n debug.phxagdc) cmds $(sysctl -n debug.phxagdc_cmds) k5 $(sysctl -n debug.phxagdc_k5) refused $(sysctl -n debug.phxagdc_refused)"
sysctl debug | grep -E "nvaccel_iop_(ok|fail|empty|src_res|src_surf|blank|ok_h0):" | tr '\n' ' '; echo
dmesg | grep -E "PhxAGDC" | awk -F"]: " '{print $2}' | sort | uniq -c | head -12
log show --start "$T0" --info --style compact --predicate 'process == "WindowServer" AND (eventMessage CONTAINS "IOPresentment" OR senderImagePath CONTAINS "IOPresentment" OR eventMessage CONTAINS "Metal compositor" OR eventMessage CONTAINS "display pipe")' 2>/dev/null | cut -c1-220 | awk '!seen[substr($0,24,150)]++' | head -12
echo "pixels: $(/Users/Shared/gpu/phxctl/phxctl vram 0 2 | tail -1) | $(/Users/Shared/gpu/phxctl/phxctl vram $(( (800*2560+1280)*4 )) 2 | tail -1)"
