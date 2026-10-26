#!/bin/bash
# 120 Hz, step 3. Round 42: the switch did not come up - the panel found no signal at 8.1 Gb/s, and when sent back to
# 4.32 Gb/s it locked at once, without any command to the PHY: the PHY most likely never left 4.32 Gb/s. A lane setting
# of swing 3 then stopped DMCUB, so the way back to 60 Hz failed too (black until reboot). Now:
#   1. DMCUB's own trace around one command that changes nothing (the lane setting the link already runs at): does its
#      firmware - the one the GOP started, not Linux's - act on the transmitter commands Linux sends it?
#   2. the whole power-off / PHY off / link training cycle at the SAME rate (60 Hz): dark ~1 s, then back. Every DMCUB
#      command and what the panel's receiver sees after the PHY is switched off and on are logged.
#   3. only if 2 came back: 120 Hz, 20 s, back to 60 Hz. No swing 3 any more; if the panel finds no signal at 8.1 Gb/s,
#      mesboot checks at once whether the PHY is still at 4.32 Gb/s, and stops.
# If the panel stays dark for more than 30 s: hold the power button; the next boot is the firmware's 60 Hz as before.
# Run: bash gpu/hz_test.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-hz120)
LOG=~/nullarc_hz120.txt
HERE=$(cd "$(dirname "$0")" && pwd)
MB=~/gpu/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
: > "$LOG"; rm -f ~/nullarc_dmcub_*.bin
say() { echo "$1"; echo "$1" >> "$LOG"; }
run() { say "\$ mesboot $*"; sudo "$MB" "$@" 2>&1 | grep -v 'never been brought up' | tee -a "$LOG"; return ${PIPESTATUS[0]}; }
snap() { sysctl -b debug.phxdmcub > ~/nullarc_dmcub_$1.bin 2>/dev/null; }
trap 'snap end; bash "$LOGS" hz120 "$LOG" ~/nullarc_dmcub_*.bin | tail -1 | tee -a "$LOG"' EXIT
say "== $(date '+%F %T') $(sw_vers -productVersion) hz_test (round 43)"
sudo -v || exit 1
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
[ "${B:-0}" -ge 41 ] || { say "STOP: PhxBringup build ${B:-?}; this needs 41"; exit 1; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot"; exit 1; }
run dpcd r 0x00115 1
if ! sudo "$MB" dpcd r 0x00115 1 2>/dev/null | grep -q ': 05'; then
    say "STOP: the link is not the firmware's 60 Hz one (LINK_RATE_SET 05 expected); reboot first"; exit 1
fi

say "== 1. DMCUB: its trace, then one command that changes nothing (SETUP_VSEMPH: pre-emphasis 1 at 4.32 Gb/s, what runs now)"
snap start
run dmubtrace 64
run dmub "8001003c 000b0804 c0a80000 010014f6"; R1=$?
snap after1
[ $R1 = 0 ] || { say "STOP: DMCUB did not take it - nothing else is tried. Did the screen change? say so."; exit 1; }

say "== 2. the same 60 Hz again, the whole cycle: in 5 s the panel goes dark for about a second, then comes back"
for i in 5 4 3 2 1; do printf '\r   %d ' $i; sleep 1; done; echo
run hz 60 again; R2=$?
snap after2
if [ $R2 != 0 ]; then
    say "== the cycle at 60 Hz did NOT come back (rc $R2). If the panel is dark: hold the power button, then boot as usual."
    say "== RESULT: step 2 failed ($R2); 120 Hz not tried"; exit 1
fi
say "== back at 60 Hz. Did the panel go dark for about a second and come back as before? say so."

say "== 3. 120 Hz in 5 s: dark for about a second, then 20 s at 120 Hz, then back to 60 Hz (dark again)"
for i in 5 4 3 2 1; do printf '\r   %d ' $i; sleep 1; done; echo
run hz 120; R3=$?
snap after3
if [ $R3 = 0 ]; then
    say "== at 120 Hz now (OTG count above). LOOK at the screen for 20 s: picture right? no flicker, no stripes, no noise?"
    for i in $(seq 20 -1 1); do printf '\r   back to 60 Hz in %2d s ' $i; sleep 1; done; echo
else
    say "== 120 Hz did NOT come up (rc $R3: 3 not trained, 4 DMCUB stopped, 5 the PHY stayed at 4.32 Gb/s): back to 60 Hz now"
fi
run hz 60; R4=$?
run dpcd r 0x00115 1
say "== RESULT: same-rate cycle ok; 120 Hz $([ $R3 = 0 ] && echo "came up" || echo "FAILED ($R3)"); back at 60 Hz $([ $R4 = 0 ] && echo ok || echo "FAILED ($R4) - reboot if the panel is dark")"
say "   tell: what did the screen show in steps 2 and 3?"
