#!/bin/bash
# 120 Hz, step 4. Round 43: the whole cycle at 60 Hz works, and so does the way back. TRANSMITTER ENABLE did switch
# the PHY to 8.1 Gb/s - but DMCUB's firmware (the one the GOP started) answers every HPD event by re-enabling the PHY
# with the link settings the GOP left it (4.32 Gb/s), and one such event comes in the middle of the rate change. Now
# mesboot moves those settings to the new rate before the switch (and back with it):
#   1. the same 60 Hz again (dark ~1 s, back) - the check that the way back still works;
#   2. only if 1 came back: 120 Hz, 20 s, back to 60 Hz.
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
say "== $(date '+%F %T') $(sw_vers -productVersion) hz_test (round 44)"
sudo -v || exit 1
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
[ "${B:-0}" -ge 41 ] || { say "STOP: PhxBringup build ${B:-?}; this needs 41"; exit 1; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot"; exit 1; }
run dpcd r 0x00115 1
if ! sudo "$MB" dpcd r 0x00115 1 2>/dev/null | grep -q ': 05'; then
    say "STOP: the link is not the firmware's 60 Hz one (LINK_RATE_SET 05 expected); reboot first"; exit 1
fi

snap start

say "== 1. the same 60 Hz again, the whole cycle: in 5 s the panel goes dark for about a second, then comes back"
for i in 5 4 3 2 1; do printf '\r   %d ' $i; sleep 1; done; echo
run hz 60 again; R2=$?
snap after1
if [ $R2 != 0 ]; then
    say "== the cycle at 60 Hz did NOT come back (rc $R2). If the panel is dark: hold the power button, then boot as usual."
    say "== RESULT: step 1 failed ($R2); 120 Hz not tried"; exit 1
fi
say "== back at 60 Hz. Did the panel go dark for about a second and come back as before? say so."

say "== 2. 120 Hz in 5 s: dark for about a second, then 20 s at 120 Hz, then back to 60 Hz (dark again)"
for i in 5 4 3 2 1; do printf '\r   %d ' $i; sleep 1; done; echo
run hz 120; R3=$?
snap after2
if [ $R3 = 0 ]; then
    say "== at 120 Hz now (OTG count above). LOOK at the screen for 20 s: picture right? no flicker, no stripes, no noise?"
    for i in $(seq 20 -1 1); do printf '\r   back to 60 Hz in %2d s ' $i; sleep 1; done; echo
else
    say "== 120 Hz did NOT come up (rc $R3: 3 not trained, 4 DMCUB stopped, 5 the PHY stayed at 4.32 Gb/s): back to 60 Hz now"
fi
run hz 60; R4=$?
run dpcd r 0x00115 1
say "== RESULT: same-rate cycle ok; 120 Hz $([ $R3 = 0 ] && echo "came up" || echo "FAILED ($R3)"); back at 60 Hz $([ $R4 = 0 ] && echo ok || echo "FAILED ($R4) - reboot if the panel is dark")"
say "   tell: what did the screen show in steps 1 and 2?"
