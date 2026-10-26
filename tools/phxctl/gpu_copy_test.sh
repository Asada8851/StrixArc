#!/bin/bash
# Strix Point: WindowServer's frames copied by SDMA into a second buffer in the carve-out that the display flips to
# (PhxBringup 36+ and PhxAccel of nullarc-strix-33+) instead of the CPU writing them through the BAR. Two steps:
#   A. on any boot: SDMA copies 1 MiB of system RAM into the carve-out and the kernel checks it (debug.phxscanout_selftest).
#      SDMA is not the queue WindowServer's Metal uses (builds 34/35 copied on that one and took WindowServer with them).
#   B. on a WindowServer-on-GPU boot: a test window animates by itself (fpsanim), 6 s with the CPU copy, then 6 s with
#      SDMA + flips. PhxBringup checks 32 pixels of every copied frame before the flip; any failure puts the firmware's
#      frame back and the CPU copies again. When B passes, the SDMA copy stays on, now and at every boot
#      ($G/phxboot-gpucopy; remove the file to go back).
# Run: bash gpu/gpu_copy_test.sh   -> everything lands on the USB stick (nullarc_logs/<time>-gpucopy)
LOG=~/nullarc_gpucopy.txt
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu
ANIM=~/gpu/metal/fpsanim; [ -x "$HERE/../metal/fpsanim" ] && ANIM=$(cd "$HERE/../metal" && pwd)/fpsanim
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
# the logs also cover build 34's WindowServer boot that hung (2026-10-26 20:00), still in the unified log
finish() { kill $KEEP 2>/dev/null; say "== done $(date '+%T')"
           sysctl -b debug.phxdmcub > ~/nullarc_dmcub.bin 2>/dev/null   # DMCUB snapshot for the 120 Hz work (read-only)
           bash "$LOGS" gpucopy "$LOG" ~/nullarc_fpsanim.out ~/nullarc_dmcub.bin | tee -a "$LOG"; }
say "== $(date '+%F %T') $(sw_vers -productVersion) gpu_copy_test"
sudo -v || exit 1
( while sleep 50; do sudo -n true 2>/dev/null || exit; done ) >/dev/null 2>&1 & KEEP=$!
trap finish EXIT
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
say "PhxBringup build ${B:-?}; scan-out copy: $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-scanout-copy" = "\(.*\)".*/\1/p' | head -1)"
say "panel: $(ioreg -rc IOFramebuffer -d0 | sed -n 's/.*<class \([A-Za-z]*\).*/\1/p' | head -1); phxboot switches: $(cd $G 2>/dev/null && ls -d phxboot-* 2>/dev/null | tr '\n' ' ')"
say "carve-out memory type (MSRs): $(ioreg -rc PhxBringup -d1 | grep -E '"phx-msr-(tom2|syscfg|mtrr-deftype|pat)"' | sed 's/^ *//' | tr '\n' ' ')"
[ "${B:-0}" -ge 36 ] || { say "STOP: PhxBringup build ${B:-?} is loaded; this needs 36+ (EFI on the USB stick)"; exit 1; }
say "== A. SDMA self-test (1 MiB system RAM -> carve-out)"
ST=$(sysctl -n debug.phxscanout_selftest 2>/dev/null)
if [ -z "$ST" ]; then say "STOP: no debug.phxscanout_selftest (the GPU is not up this boot: tail /var/log/phxboot.log)"; exit 1; fi
[ "$ST" = 0 ] && { sudo sysctl -w debug.phxscanout_selftest=1 >/dev/null 2>&1; ST=$(sysctl -n debug.phxscanout_selftest); }
case $ST in 1) say "   passed" ;; 2) say "   FAILED: the copy came back wrong" ;; 3) say "   FAILED: SDMA did not finish (reset; GPU copies off until reboot)" ;; *) say "   could not run ($ST)" ;; esac
log show --last 5m --style compact --predicate 'eventMessage CONTAINS "SDMA"' 2>/dev/null | sed -E -n 's/.*(PhxBringup: .*)/\1/p' | tail -4 | while IFS= read -r l; do say "   | $l"; done
say "   state: $(sysctl -n debug.phxscanout_state 2>/dev/null)"
[ "$ST" = 1 ] || { say "== RESULT: self-test failed; nothing else is tried"; exit 1; }
if ! ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB; then
    say "== A passed. Step B needs WindowServer on the GPU: sudo bash install_boot.sh --windowserver --force, reboot, run this again"
    exit 0
fi
sysctl -n debug.nvaccel_iop_gpu_busy >/dev/null 2>&1 || { say "STOP: PhxAccel is not the one of nullarc-strix-33: sudo bash update_accel.sh, reboot"; exit 1; }
[ -x "$ANIM" ] || { say "STOP: no fpsanim"; exit 1; }
say "== B. WindowServer's frames (keep hands off the mouse; a test window animates by itself; watch it for garbage or tearing)"
G0=$(sysctl -n debug.nvaccel_iop_gpucopy)
WSP=$(pgrep -x WindowServer | head -1)
K="nvaccel_iop_ok nvaccel_iop_copy_us_sum nvaccel_iop_gap1 nvaccel_iop_gap2 nvaccel_iop_gap3 nvaccel_iop_gpu_copies
   nvaccel_iop_gpu_fail nvaccel_iop_gpu_busy phxscanout_copies phxscanout_us_sum phxscanout_busy"
snap() { for k in $K; do printf '%s=%s ' $k "$(sysctl -n debug.$k 2>/dev/null || echo 0)"; done; }
val() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
PASSED=0
: > ~/nullarc_fpsanim.out
mode() {   # mode <label> <gpucopy>
    sudo sysctl -w debug.nvaccel_iop_gpucopy=$2 >/dev/null
    echo "== $1" >> ~/nullarc_fpsanim.out
    "$ANIM" 9 >> ~/nullarc_fpsanim.out 2>&1 & local ap=$!
    sleep 2
    # the test window did not show on the 880M once (2026-10-26): then the frames come from you
    kill -0 $ap 2>/dev/null || say "   (the test window did not start - see nullarc_fpsanim.out; DRAG A WINDOW AROUND for the next 6 s)"
    local a b tp; a=$(snap)
    top -l 4 -s 1 -stats pid,cpu -pid $WSP 2>/dev/null | awk '$1 ~ /^[0-9]+$/' | tail -1 > ~/nullarc_gc_top.txt & tp=$!
    sleep 6; b=$(snap); wait $ap 2>/dev/null; wait $tp 2>/dev/null   # not a bare wait: the sudo keeper never ends
    for k in $K; do x=$(val "$b" $k); y=$(val "$a" $k); eval "D_$k=$(( ${x:-0} - ${y:-0} ))"; done
    say "-- $1"
    say "   frames $D_nvaccel_iop_ok in 6 s = $(awk -v f=$D_nvaccel_iop_ok 'BEGIN{printf "%.1f", f/6}') fps (gaps 1/2/3: $D_nvaccel_iop_gap1/$D_nvaccel_iop_gap2/$D_nvaccel_iop_gap3); WindowServer $(awk '{print $2}' ~/nullarc_gc_top.txt)% CPU"
    say "   per frame: $(awk -v u=$D_nvaccel_iop_copy_us_sum -v n=$D_nvaccel_iop_ok 'BEGIN{ if (n) printf "%.2f", u/n/1000; else print "-" }') ms in the display pipe; SDMA frames $D_nvaccel_iop_gpu_copies (refused $D_nvaccel_iop_gpu_fail, dropped busy $D_nvaccel_iop_gpu_busy), kernel $(awk -v u=$D_phxscanout_us_sum -v n=$D_phxscanout_copies 'BEGIN{ if (n) printf "%.2f ms", u/n/1000; else print "-" }') a frame"
    say "   state: $(sysctl -n debug.phxscanout_state)"
    if [ $2 = 1 ]; then
        if [ $D_nvaccel_iop_gpu_copies -gt 0 ] && [ $D_nvaccel_iop_gpu_fail = 0 ] && [ "$(sysctl -n debug.phxscanout_state)" = on ]; then
            PASSED=1; say "   PASSED"
        else say "   not passed"; fi
    fi
}
mode "CPU copy" 0
mode "SDMA copy + flip" 1
say "== messages this boot"
log show --last 3m --style compact --predicate 'eventMessage CONTAINS "GPU copy" OR eventMessage CONTAINS "scan-out" OR eventMessage CONTAINS "SDMA"' 2>/dev/null |
    sed -E -n 's/.*((NMXAccel|PhxBringup|PhxAccel)[^:]*: .*)/\1/p' | awk '!seen[$0]++' | head -12 | while IFS= read -r l; do say "   | $l"; done
if [ $PASSED = 1 ]; then
    sudo touch $G/phxboot-gpucopy
    say "== RESULT: the SDMA copy works; it stays on now and from every boot ($G/phxboot-gpucopy; sudo rm it to go back)."
    say "   Did the test window look right (no garbage, no tearing)? Use the Mac a bit: anything odd -> bash ~/gpu/nullarc_logs.sh odd"
else
    sudo sysctl -w debug.nvaccel_iop_gpucopy=$G0 >/dev/null 2>&1
    say "== RESULT: the SDMA copy did not pass; frames stay CPU-copied."
fi
