#!/bin/bash
# 120 Hz with WindowServer: on a boot where phxboot switched the panel to 120 Hz before WindowServer opened it
# ($G/phxboot-120), does everything agree it is 120 Hz - the link, the display engine, what PhxAMDFB reports, macOS's
# display settings - and do the VBL ticks and the frames presented (SDMA copies) run at 120 a second?
#   1. what phxboot did at boot ($G/logs/hz120-boot.txt, /var/log/phxboot.log)
#   2. the link (DPCD LINK_RATE_SET, lane status), PhxAMDFB's refresh, system_profiler's
#   3. 5 s hands off, then 10 s of your own use (drag a window around, scroll): ticks, frames presented, SDMA copies,
#      dropped frames, underflow - second by second
# fps_check.sh (the next test) then measures with the animated window, as before.
# Run: bash gpu/hz_ws_check.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-hzws)
LOG=~/nullarc_hzws.txt
G=/Users/Shared/gpu
HERE=$(cd "$(dirname "$0")" && pwd)
MB=$G/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
trap 'bash "$LOGS" hzws "$LOG" | tail -1 | tee -a "$LOG"' EXIT
say "== $(date '+%F %T') $(sw_vers -productVersion) hz_ws_check"
sudo -v || exit 1
ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB || { say "STOP: PhxAMDFB does not have the panel this boot"; exit 1; }

say "== 1. at boot"
say "   120 Hz at boot: $([ -e $G/phxboot-120 ] && echo on || echo "OFF (phxboot removed it after a failed switch, or it was never on)")"
grep -E '120 Hz|60 Hz|hz120' /var/log/phxboot.log | tail -4 | sed 's/^/   phxboot: /' | while IFS= read -r l; do say "$l"; done
[ -f $G/logs/hz120-boot.txt ] && grep -E '^hz: (done|STOP|clock|equal|probe|CR|EQ)|not taken' $G/logs/hz120-boot.txt | sed 's/^/   /' | while IFS= read -r l; do say "$l"; done

say "== 2. now"
say "   $(sudo "$MB" dpcd r 0x00115 1 2>/dev/null | grep dpcd) (07 = 8.1 Gb/s, 120 Hz; 05 = 4.32, 60 Hz)"
say "   $(sudo "$MB" dpcd r 0x00202 3 2>/dev/null | grep dpcd) (lane status: 77 77 01 = locked)"
say "   PhxAMDFB reports $(sysctl -n debug.phxfb_remeasure 2>/dev/null) mHz; ioreg PhxFBRefreshMilliHz $(ioreg -rc PhxAMDFB -d1 | sed -n 's/.*"PhxFBRefreshMilliHz" = \([0-9]*\).*/\1/p' | head -1)"
system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Resolution|Refresh|UI Looks' | sed 's/^ */   macOS: /' | while IFS= read -r l; do say "$l"; done
say "   SDMA: $(sysctl -n debug.phxscanout_state 2>/dev/null), $(sysctl -n debug.phxscanout_copies 2>/dev/null) frames so far"

get() { sysctl -n debug.$1 2>/dev/null || echo 0; }
snap() { echo "$(get phxfb_vbl_n) $(get nvaccel_iop_ok) $(get phxscanout_copies) $(get nvaccel_iop_gpu_busy) $(get phxscanout_moving)"; }
watch() {   # watch <seconds>: per second, VBL ticks / frames presented / SDMA copies / busy drops / moving drops (bash 3.2)
    local i a b
    a=$(snap)
    say "   second: ticks presented sdma busy moving"
    for i in $(seq 1 $1); do
        sleep 1
        b=$(snap)
        say "   $(echo "$i $a $b" | awk '{printf "%6d: %5d %9d %4d %4d %6d", $1, $7-$2, $8-$3, $9-$4, $10-$5, $11-$6}')"
        a=$b
    done
}
say "== 3a. 5 s hands off (the desktop idle: few frames are normal)"
watch 5
say "== 3b. 10 s of your own use: drag a window around in circles, or scroll a long page - starts in 3 s"
for i in 3 2 1; do printf '\r   %d ' $i > /dev/tty; sleep 1; done; printf '\r   go!\n' > /dev/tty
watch 10
say "   SDMA now: $(sysctl -n debug.phxscanout_state 2>/dev/null) (an underflow would have turned it off); pixel-check drops $(get phxscanout_mismatch)"
say "== done. tell: did dragging feel smoother than at 60 Hz? any tearing, stripes, flicker?"
