#!/bin/bash
# 120 Hz, step 2: the panel switched to 120 Hz by `mesboot hz 120` (Linux DC's own sequence on this 880M, with the
# firmware's 3K@60 values for what depends on our linear surface), held 20 s, then put back to 60 Hz by `mesboot hz 60`
# - always, also when 120 Hz did not work. The panel goes dark for about a second each time (DC power-cycles it too).
# If it stays dark for more than 30 s: hold the power button; the next boot is the firmware's 60 Hz as before.
# WindowServer keeps pacing at 60 Hz in this step (PhxAMDFB still reports 60): this checks the panel and the link.
# Run: bash gpu/hz_test.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-hz120)
LOG=~/nullarc_hz120.txt
HERE=$(cd "$(dirname "$0")" && pwd)
MB=~/gpu/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
run() { say "\$ mesboot $*"; sudo "$MB" "$@" 2>&1 | grep -v 'never been brought up' | tee -a "$LOG"; return ${PIPESTATUS[0]}; }
rundbg() { say "\$ AUX_DEBUG=1 mesboot $*"; sudo env AUX_DEBUG=1 "$MB" "$@" 2>&1 | grep -v 'never been brought up' | tee -a "$LOG"; return ${PIPESTATUS[0]}; }
trap 'bash "$LOGS" hz120 "$LOG" | tail -1 | tee -a "$LOG"' EXIT
say "== $(date '+%F %T') $(sw_vers -productVersion) hz_test"
sudo -v || exit 1
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
[ "${B:-0}" -ge 41 ] || { say "STOP: PhxBringup build ${B:-?}; this needs 41"; exit 1; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot"; exit 1; }
say "== AUX check (replies placed by their index since round 42; raw words shown): DPCD rev 14, LINK_RATE_SET 05"
rundbg dpcd r 0x00000 1; rundbg dpcd r 0x00115 1; rundbg dpcd r 0x00010 16
if ! sudo "$MB" dpcd r 0x00115 1 2>/dev/null | grep -q ': 05'; then
    say "STOP: DPCD reads do not come back right yet (raw words above); nothing was switched"; exit 1
fi
say "== 120 Hz in 5 s: the panel goes dark for about a second, then 20 s at 120 Hz, then back to 60 Hz (dark again)"
for i in 5 4 3 2 1; do printf '\r   %d ' $i; sleep 1; done; echo
run hz 120; RC=$?
if [ $RC = 0 ]; then
    say "== at 120 Hz now (OTG count above). LOOK at the screen for 20 s: picture right? no flicker, no stripes, no noise?"
else
    say "== 120 Hz did NOT come up (rc $RC): going back to 60 Hz now"
fi
[ $RC = 0 ] && for i in $(seq 20 -1 1); do printf '\r   back to 60 Hz in %2d s ' $i; sleep 1; done; echo
run hz 60; RC60=$?
run dpcd r 0x00115 1
say "== RESULT: 120 Hz $([ $RC = 0 ] && echo "came up" || echo "FAILED ($RC)"); back at 60 Hz $([ $RC60 = 0 ] && echo ok || echo "FAILED ($RC60) - reboot if the panel is wrong")"
say "   tell: what did the screen show at 120 Hz?"
