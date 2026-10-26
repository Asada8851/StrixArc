#!/bin/bash
# WindowServer on the GPU (M2) feels like ~30 Hz: where do the frames go? Run on a boot where PhxAMDFB has the panel.
# The display pipe copies each composited frame by CPU into the scan-out (nvrm-accel.cpp performTransaction) and, with
# debug.nvaccel_iop_vsync=1, completes it on the next VBL tick of PhxAMDFB's software timer. A frame that takes longer
# than one refresh to come out of WindowServer + copy waits for the tick after: every other tick, half the rate.
# Measured over 6 s of continuous Terminal output (WindowServer redraws at its own pace while it lasts):
#   frames presented, refresh ticks, copy time per frame, gaps between frames in refreshes (1 = full rate, 2 = half),
#   VBL waits, how late the ticks fire, WindowServer's waits for its composites; then the same with vsync off and with
#   the other copy loop (SSE on/off), one setting at a time, each put back at the end. Nothing else is changed.
# Run: bash fps_check.sh (from the package or ~/gpu)  ->  send ~/nullarc_fps.txt
# bash fps_check.sh --live [seconds]: the same counters over your own use of the desktop (default 20 s), second by second
LOG=~/nullarc_fps.txt
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB || { say "STOP: PhxAMDFB does not have the panel on this boot (not an M2 boot)"; exit 1; }
sudo -v || exit 1
K="nvaccel_iop_ok nvaccel_iop_src_surf nvaccel_iop_src_res nvaccel_iop_copy_us_sum nvaccel_iop_gap1 nvaccel_iop_gap2
   nvaccel_iop_gap3 nvaccel_iop_gap4 nvaccel_iop_phase_us_sum nvaccel_iop_vsync_waits nvaccel_iop_vsync_late
   nvaccel_iop_copy_skip nvaccel_iop_blank nvaccel_iop_fail nvaccel_iop_flips phxfb_vbl_n phxfb_vbl_late_us_sum
   nvaccel_surface_gpu_waits nvaccel_surface_gpu_timeouts nvaccel_iop_gpu_copies nvaccel_iop_gpu_fail nvaccel_iop_gpu_busy
   phxscanout_copies phxscanout_us_sum phxscanout_mismatch phxscanout_moving phxscanout_gpuwait_us phxscanout_gpulate"
snap() { for k in $K; do printf '%s=%s ' "$k" "$(sysctl -n debug.$k 2>/dev/null || echo 0)"; done; }
val() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
# continuous drawing in this Terminal: a line every 4 ms for <seconds>, to the terminal, not the log
animate() { perl -e '$|=1; $e=time+$ARGV[0]; $i=0; while (time < $e) { printf "\r  drawing %6d %s", $i++, "#" x ($i % 60) . " " x 60; select(undef,undef,undef,0.004) }' "$1" > /dev/tty; printf '\r%80s\r' '' > /dev/tty; }
# fpsanim (next to this script, or in ~/gpu/metal): a window whose square moves every 1/120 s, a frame for WindowServer at
# every refresh; without it, the Terminal scroll above. WindowServer's CPU and a stack sample are taken during the first.
HERE=$(cd "$(dirname "$0")" && pwd)
ANIM=~/gpu/metal/fpsanim; [ -x "$HERE/../metal/fpsanim" ] && ANIM=$(cd "$HERE/../metal" && pwd)/fpsanim
[ -x "$ANIM" ] || ANIM=""
WSP=$(pgrep -x WindowServer | head -1)
# --live: the user drives the desktop; frames presented each second, and each second the slowest frame copy since boot
# grew (a stall long enough to see as a jerk)
live_watch() {
    local i f0 f1 m0 m1 per=""
    f0=$(sysctl -n debug.nvaccel_iop_ok); m0=$(sysctl -n debug.nvaccel_iop_copy_us_max)
    for i in $(seq 1 $1); do
        sleep 1
        f1=$(sysctl -n debug.nvaccel_iop_ok); m1=$(sysctl -n debug.nvaccel_iop_copy_us_max)
        per="$per $(( f1 - f0 ))"
        [ "$m1" -gt "$m0" ] && say "   second $i: a frame copy took $(( m1 / 1000 )) ms (the slowest since boot)"
        f0=$f1; m0=$m1
    done
    say "   frames presented, second by second:$per"
}
measure() {   # measure <label> <tag> [profile]
    local a b d s=${MS:-6} ap=""
    if [ -n "$ANIM" ] && [ -z "$LIVE" ]; then
        "$ANIM" $(( s + 3 )) > ~/nullarc_fpsanim.out 2>&1 & ap=$!
        sleep 2
    fi
    a=$(snap)
    if [ -n "${3:-}" ] && [ -n "$WSP" ]; then
        top -l 4 -s 1 -stats pid,cpu,threads,command -pid $WSP 2>/dev/null | awk '$1 ~ /^[0-9]+$/' | tail -1 > ~/nullarc_ws_top.txt &
        sudo sample $WSP 3 -file ~/nullarc_ws_sample.txt >/dev/null 2>&1 &
    fi
    if [ -n "$LIVE" ]; then live_watch $s; elif [ -n "$ANIM" ]; then sleep $s; else animate $s; fi
    b=$(snap)
    [ -n "$ap" ] && wait $ap
    for k in $K; do x=$(val "$b" $k); y=$(val "$a" $k); eval "D_$k=$(( ${x:-0} - ${y:-0} ))"; done
    local fr=$D_nvaccel_iop_ok cp=$(( D_nvaccel_iop_src_surf + D_nvaccel_iop_src_res ))
    local gaps=$(( D_nvaccel_iop_gap1 + D_nvaccel_iop_gap2 + D_nvaccel_iop_gap3 + D_nvaccel_iop_gap4 ))
    say "-- $1"
    say "   frames presented: $fr in ${s} s = $(awk -v f=$fr -v s=$s 'BEGIN{printf "%.1f", f/s}') fps; refresh ticks $(awk -v f=$D_phxfb_vbl_n -v s=$s 'BEGIN{printf "%.1f", f/s}')/s"
    say "   copy: $(awk -v u=$D_nvaccel_iop_copy_us_sum -v n=$cp 'BEGIN{ if (n) printf "%.2f ms", u/n/1000; else print "-" }') per frame ($cp copied, skipped $D_nvaccel_iop_copy_skip, blank $D_nvaccel_iop_blank, failed $D_nvaccel_iop_fail, zero-copy $D_nvaccel_iop_flips)"
    [ $(( D_nvaccel_iop_gpu_copies + D_nvaccel_iop_gpu_fail + D_nvaccel_iop_gpu_busy )) -gt 0 ] &&
        say "   SDMA: $D_nvaccel_iop_gpu_copies frames copied + flipped ($(awk -v u=$D_phxscanout_us_sum -v n=$D_phxscanout_copies 'BEGIN{ if (n) printf "%.2f ms", u/n/1000; else print "-" }') each in the kernel), refused $D_nvaccel_iop_gpu_fail, dropped busy $D_nvaccel_iop_gpu_busy (of them a pixel check $D_phxscanout_mismatch, a frame still being drawn $D_phxscanout_moving); waited for the GPU $(awk -v u=$D_phxscanout_gpuwait_us -v n=$D_phxscanout_copies 'BEGIN{ if (n) printf "%.2f ms", u/n/1000; else print "-" }') a frame (late $D_phxscanout_gpulate); state: $(sysctl -n debug.phxscanout_state 2>/dev/null)"
    say "   gaps between frames, in refreshes: 1: $D_nvaccel_iop_gap1  2: $D_nvaccel_iop_gap2  3: $D_nvaccel_iop_gap3  4+: $D_nvaccel_iop_gap4"
    say "   frame start after its tick: $(awk -v u=$D_nvaccel_iop_phase_us_sum -v n=$gaps 'BEGIN{ if (n) printf "%.2f ms", u/n/1000; else print "-" }') on average; vsync waits $D_nvaccel_iop_vsync_waits, late completions $D_nvaccel_iop_vsync_late"
    say "   VBL tick lateness: $(awk -v u=$D_phxfb_vbl_late_us_sum -v n=$D_phxfb_vbl_n 'BEGIN{ if (n) printf "%.0f us", u/n; else print "-" }') on average; WindowServer composite waits $D_nvaccel_surface_gpu_waits (timeouts $D_nvaccel_surface_gpu_timeouts)"
    [ -n "$ap" ] && say "   app side: $(tail -1 ~/nullarc_fpsanim.out)"
    if [ -n "${3:-}" ] && [ -n "$WSP" ]; then
        wait
        say "   WindowServer (pid $WSP): $(awk '{print $2 "% CPU, " $3 " threads"}' ~/nullarc_ws_top.txt 2>/dev/null)"
        say "   WindowServer, where its threads were (sample, top of stack, heaviest first):"
        sed -n '/Sort by top of stack/,/Binary Images/p' ~/nullarc_ws_sample.txt 2>/dev/null | grep -E '\(in ' | head -18 |
            sed 's/^ */     /' | cut -c1-150 | while IFS= read -r l; do say "$l"; done
    fi
    eval "FPS_$2=$fr"
}
VS0=$(sysctl -n debug.nvaccel_iop_vsync); SSE0=$(sysctl -n debug.nvaccel_iop_sse); AL0=$(sysctl -n debug.phxfb_alias)
GC0=$(sysctl -n debug.nvaccel_iop_gpucopy 2>/dev/null)
restore() { sudo sysctl -w debug.nvaccel_iop_vsync=$VS0 debug.nvaccel_iop_sse=$SSE0 debug.phxfb_alias=$AL0 >/dev/null 2>&1
            [ -n "$GC0" ] && sudo sysctl -w debug.nvaccel_iop_gpucopy=${GC_KEEP:-$GC0} >/dev/null 2>&1; }
LOGS=~/gpu/nullarc_logs.sh; [ -f "$(dirname "$0")/nullarc_logs.sh" ] && LOGS="$(dirname "$0")/nullarc_logs.sh"
finish() { restore; [ -f "$LOGS" ] && bash "$LOGS" fps "$LOG" ~/nullarc_fpsanim.out | tee -a "$LOG"; }
trap finish EXIT

say "== $(date '+%F %T') $(sw_vers -productVersion) fps_check"
say "   panel: $(system_profiler SPDisplaysDataType 2>/dev/null | sed -n 's/^ *Resolution: //p' | head -1)"
VIEW=$(sysctl -n debug.phxfb_alias_view 2>/dev/null)
say "   settings: vsync $VS0, sse $SSE0, alias $AL0, vbl_staged $(sysctl -n debug.phxfb_vbl_staged), bos_flush $(sysctl -n debug.phxbos_flush 2>/dev/null)"
PB=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
say "   SDMA copy: PhxBringup build ${PB:-?}, debug.nvaccel_iop_gpucopy=${GC0:-absent}, state: $(sysctl -n debug.phxscanout_state 2>/dev/null || echo absent), frames so far $(sysctl -n debug.phxscanout_copies 2>/dev/null || echo 0), at boot: $([ -e /Users/Shared/gpu/phxboot-gpucopy ] && echo on || echo off)"
say "   pixel-check drops so far $(sysctl -n debug.phxscanout_mismatch 2>/dev/null || echo -), last: $(sysctl -n debug.phxscanout_lastbad 2>/dev/null || echo -)"
if [ -n "$VIEW" ]; then
    say "   scan-out write speed (PhxAccel's probe at the first frame): BAR $(sysctl -n debug.phxfb_mbps_bar) MB/s, carve-out write-combined $(sysctl -n debug.phxfb_mbps_wc) MB/s, write-back $(sysctl -n debug.phxfb_mbps_wb) MB/s; frames go through view $VIEW (0 BAR, 1 WC, 2 WB)"
else
    say "   (this PhxAccel has no write-speed probe: sudo bash update_accel.sh from nullarc-strix-21, then reboot)"
fi
say "   WindowServer composite wait max $(sysctl -n debug.nvaccel_surface_gpu_wait_max_us 2>/dev/null) us, slowest copy so far $(sysctl -n debug.nvaccel_iop_copy_us_max) us"
say "== PhxFB / display pipe messages this boot"
BOOTS=$(date -r "$(sysctl -n kern.boottime | sed -n 's/.*sec = \([0-9]*\),.*/\1/p')" '+%Y-%m-%d %H:%M:%S')
log show --start "$BOOTS" --style compact --predicate 'eventMessage CONTAINS "PhxFB" OR eventMessage CONTAINS "iop:"' 2>/dev/null |
    sed -E -n 's/.*((PhxFB|PhxAccel|NMXAccel)[^:]*: .*)/\1/p' | grep -E 'alias|refresh|panel published|console|BAR index|VRAM pool|iop: (K6|bench|first)' |
    awk '!seen[$0]++' | head -24 | while IFS= read -r l; do say "   | $l"; done
sudo sysctl -w debug.nvaccel_iop_bench=1 >/dev/null; animate 1
say "   reading one frame's source: $(sysctl -n debug.nvaccel_iop_read_us) us (bench)"

if [ "${1:-}" = --live ]; then
    LIVE=1; MS=${2:-20}
    say "== live: $MS s of your own use (drag windows, Launchpad, Mission Control, scrolling - whatever felt jerky)"
    for i in 5 4 3 2 1; do printf '\r   starts in %d s ' $i > /dev/tty; sleep 1; done; printf '\r   go!            \n' > /dev/tty
    measure "live, $MS s" live profile
    restore
    for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && cp "$LOG" "$v/" && say "(log copied to $v)"; done
    say "== done $(date '+%T'). Send $LOG"; exit 0
fi
say "== measurements (keep hands off the mouse; $( [ -n "$ANIM" ] && echo "a test window animates by itself" || echo "this Terminal window scrolls by itself"))"
measure "as set now (vsync $VS0, sse $SSE0, gpucopy ${GC0:-0})" now profile
# SDMA copies the frame into a second carve-out buffer and the display flips to it (PhxBringup 36+); kept on when it
# works, now and at every boot ($G/phxboot-gpucopy)
if [ -n "$GC0" ] && [ "${PB:-0}" -ge 36 ] && [ "$GC0" = 0 ]; then
    sudo sysctl -w debug.nvaccel_iop_gpucopy=1 >/dev/null
    measure "SDMA copy + flip (only this changed)" sdma
    if [ "$D_nvaccel_iop_gpu_copies" -gt 0 ] && [ "$D_nvaccel_iop_gpu_fail" = 0 ] && [ "$(sysctl -n debug.phxscanout_state)" = on ]; then
        GC_KEEP=1; sudo touch /Users/Shared/gpu/phxboot-gpucopy
        say "   => SDMA works: it stays on, now and at every boot (sudo rm /Users/Shared/gpu/phxboot-gpucopy to go back)"
    else
        say "   => SDMA did not pass: frames stay CPU-copied"
    fi
    sudo sysctl -w debug.nvaccel_iop_gpucopy=0 >/dev/null
fi
sudo sysctl -w debug.nvaccel_iop_vsync=$(( 1 - VS0 )) >/dev/null
measure "vsync $(( 1 - VS0 )) (only this changed)" vs
sudo sysctl -w debug.nvaccel_iop_vsync=$VS0 debug.nvaccel_iop_sse=$(( 1 - SSE0 )) >/dev/null
measure "sse $(( 1 - SSE0 )) (only this changed)" sse
if [ "${VIEW:-0}" != 0 ]; then
    sudo sysctl -w debug.nvaccel_iop_sse=$SSE0 debug.phxfb_alias=$(( 1 - AL0 )) >/dev/null
    measure "phxfb_alias $(( 1 - AL0 )) (1 = the carve-out view, 0 = the BAR; only this changed)" alias
fi
restore
say "   settings back: vsync $(sysctl -n debug.nvaccel_iop_vsync), sse $(sysctl -n debug.nvaccel_iop_sse), alias $(sysctl -n debug.phxfb_alias), gpucopy $(sysctl -n debug.nvaccel_iop_gpucopy 2>/dev/null)"
for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
