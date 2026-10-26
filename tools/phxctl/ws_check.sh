#!/bin/bash
# After the boot `install_boot.sh --ws-trial` armed: did WindowServer come up on the 880M, and does it keep drawing?
#  - who has the panel (PhxAMDFB or the firmware framebuffer) and, if not PhxAMDFB, why (its kernel messages);
#  - phxboot's M2 run in /var/log/phxboot.log (arming, WindowServer pid, frames, "done");
#  - frames WindowServer presents through the GPU (debug.nvaccel_iop_ok) while this script scrolls the Terminal;
#  - WindowServer crash or watchdog reports since boot;
#  - Metal for an app beside WindowServer: compute, a draw, 30 s of `mtlprobe stress`, and frames still flowing after it.
# Passed: /Users/Shared/gpu/ws-trial-ok, and `sudo bash install_boot.sh --windowserver` makes it every boot.
# Run: bash ~/gpu/ws_check.sh  ->  send ~/nullarc_ws_check.txt
LOG=~/nullarc_ws_check.txt
G=/Users/Shared/gpu; DR=/Library/Logs/DiagnosticReports
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; sync; }
FAILS=""
bad() { say "   FAIL: $1"; FAILS="$FAILS
  - $1"; }
ok()  { say "   ok: $1"; }
HERE=$(cd "$(dirname "$0")" && pwd)
MP=~/gpu/metal/mtlprobe; [ -x "$HERE/../metal/mtlprobe" ] && MP=$(cd "$HERE/../metal" && pwd)/mtlprobe
iop() { sysctl -n debug.nvaccel_iop_ok 2>/dev/null || echo 0; }
# scroll this Terminal for <seconds> so WindowServer has something to present; frames counted around it
frames() {
    local a b t=0; a=$(iop)
    while [ $t -lt $(( $1 * 10 )) ]; do printf '\r   drawing... %3d%%' $(( t * 10 / $1 )) >&2; sleep 0.1; t=$((t + 1)); done; printf '\r' >&2
    b=$(iop); echo $(( b - a ))
}
say "== $(date '+%F %T') $(sw_vers -productVersion) ws_check"
BOOT=$(sysctl -n kern.boottime | sed -n 's/.*sec = \([0-9]*\),.*/\1/p')
BOOTS=$(date -r "$BOOT" '+%Y-%m-%d %H:%M:%S')
say "   booted $BOOTS; trial armed: $(cat $G/ws-trial 2>/dev/null || echo 'no ($G/ws-trial missing)')"
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
say "   PhxBringup build ${B:-not loaded}"
sudo -v || exit 1

say "== who has the panel"
FBC=$(ioreg -rc IOFramebuffer -d0 | sed -n 's/.*<class \([A-Za-z0-9_]*\),.*/\1/p' | sort | uniq -c | tr '\n' ' ')
say "   framebuffers: ${FBC:-none}"
M2=0; ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && M2=1
say "   PhxAMDFB / PhxAGDC kernel messages this boot:"
log show --start "$BOOTS" --style compact --predicate 'eventMessage CONTAINS "PhxFB" OR eventMessage CONTAINS "PhxAGDC"' 2>/dev/null |
    sed -E -n 's/.*((PhxFB|PhxAGDC):.*)/\1/p' | awk '!seen[$0]++' | head -25 | while IFS= read -r l; do say "     | $l"; done
if [ $M2 = 1 ]; then ok "PhxAMDFB has the panel"
else
    bad "PhxAMDFB did not take the panel: the firmware framebuffer drew this boot (its reason is in the messages above)"
fi
if nvram phxfb-boot >/dev/null 2>&1; then say "   NVRAM phxfb-boot: set (the next boot takes the panel again)"
else say "   NVRAM phxfb-boot: not set (the trial used it up)"; fi

say "== phxboot this boot"
HDR=$(sudo grep -n '^== [0-9-]* [0-9:]* phxboot' /var/log/phxboot.log 2>/dev/null | tail -1); HN=${HDR%%:*}
[ -n "$HN" ] && sudo tail -n +"$HN" /var/log/phxboot.log | head -40 | while IFS= read -r l; do say "     | $l"; done
PB=$( [ -n "$HN" ] && sudo tail -n +"$HN" /var/log/phxboot.log )
echo "$PB" | grep -q 'M2 failed' && bad "phxboot gave up on M2: $(echo "$PB" | grep 'M2 failed' | head -1)"
echo "$PB" | grep -q '^previous attempt did not finish' && bad "the boot before did not finish the GPU bring-up: phxboot turned itself off ($G/phxboot-off)"
if [ $M2 = 1 ]; then
    echo "$PB" | grep -q '^armed and released' && ok "phxboot armed the display and released the GPU" || bad "phxboot did not arm the display"
    echo "$PB" | grep -q '^done ' && ok "phxboot finished" || bad "phxboot did not reach 'done'"
fi
say "   agdc $(sysctl -n debug.phxagdc 2>/dev/null) cmds $(sysctl -n debug.phxagdc_cmds 2>/dev/null) refused $(sysctl -n debug.phxagdc_refused 2>/dev/null)"
say "   $(sysctl debug 2>/dev/null | grep -E 'nvaccel_iop_(ok|fail|empty|blank):' | tr '\n' ' ')"

say "== WindowServer"
WS=$(pgrep -x WindowServer); say "   pid ${WS:-none}, up $(ps -o etime= -p ${WS:-1} 2>/dev/null | tr -d ' ')"
NR=$(find $DR -name 'WindowServer*' -newermt "$BOOTS" 2>/dev/null | wc -l | tr -d ' ')
[ "$NR" = 0 ] && ok "no WindowServer crash or hang reports since boot" || {
    bad "$NR WindowServer report(s) since boot"; find $DR -name 'WindowServer*' -newermt "$BOOTS" | head -5 | while IFS= read -r l; do say "     $l"; done; }
if [ $M2 = 1 ]; then
    F=$(frames 4)
    [ "${F:-0}" -gt 0 ] && ok "$F frames presented through the GPU in 4 s of scrolling" || bad "no frames presented through the GPU (debug.nvaccel_iop_ok did not move)"
fi

say "== Metal beside WindowServer"
if [ -x "$MP" ]; then
    timeout_run() { local lim=$1; shift; "$@" > ~/nullarc_wsc.out 2>&1 & local p=$! t=0
        while kill -0 $p 2>/dev/null && [ $t -lt $lim ]; do sleep 1; t=$((t+1)); done
        if kill -0 $p 2>/dev/null; then kill -9 $p 2>/dev/null; RC=124; else wait $p; RC=$?; fi
        tail -3 ~/nullarc_wsc.out | while IFS= read -r l; do say "     $l"; done; }
    timeout_run 60 $MP;          [ $RC = 0 ] && ok "compute" || bad "Metal compute beside WindowServer (rc $RC)"
    timeout_run 60 $MP render;   [ $RC = 0 ] && ok "draw" || bad "Metal draw beside WindowServer (rc $RC)"
    timeout_run 120 $MP stress 30; grep -q 'all correct' ~/nullarc_wsc.out || { [ $RC = 0 ] && RC=99; }
    [ $RC = 0 ] && ok "stress 30 s" || bad "stress beside WindowServer (rc $RC)"
    if [ $M2 = 1 ]; then
        F=$(frames 3)
        [ "${F:-0}" -gt 0 ] && ok "$F frames after the load: the desktop still draws" || bad "no frames after the load"
    fi
else
    bad "no $MP (bash install.sh from the package)"
fi

say "== summary"
if [ -z "$FAILS" ] && [ $M2 = 1 ]; then
    date '+%F %T ws_check passed' | sudo tee $G/ws-trial-ok > /dev/null
    say "PASSED: WindowServer runs on the 880M."
    say "every boot from now on:  sudo bash install_boot.sh --windowserver"
    say "(as it is now, the next boot is back on the firmware framebuffer: the trial armed one boot only)"
else
    sudo rm -f $G/ws-trial-ok
    say "NOT PASSED:$FAILS"
    say "The next boot is on the firmware framebuffer by itself (nothing re-arms it). Send this log."
    [ -e $G/phxboot-off ] && say "phxboot is off after an unfinished boot; once the log is sent: sudo rm $G/phxboot-off"
fi
for v in /Volumes/*; do [ -e "$v/nullarc-strix-19.zip" ] && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
