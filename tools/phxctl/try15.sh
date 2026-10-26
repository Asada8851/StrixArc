#!/bin/bash
# Strix Point, step 15: the check before WindowServer goes on the GPU. Runs on a boot where phxboot (the LaunchDaemon
# from install_boot.sh) brought the GPU up; it does no bring-up of its own, and refuses to run where the GPU is not up.
#  1. Boot path: PhxBringup build >= 33, this boot's /var/log/phxboot.log ended with "done", NMXAccel runs, the
#     plugin name is back on it, the GCVM fault redirect is on (GCVM_L2_PROTECTION_FAULT_CNTL = 0x3ffffffc).
#  2. Display, read only: the panel's mode (system_profiler), OTG0 on DCN 3.5 (totals, enable bit, refresh measured
#     from its frame counter, as PhxAMDFB will), the firmware framebuffer's owner, NVRAM phxfb-boot not left armed,
#     automatic login (the login window may come up black on the GPU).
#  3. Metal, as WindowServer will use it: compute, a draw, `mtlprobe stress` (display-sized textures, blits, blending,
#     compositing, blur, IOSurfaces both ways, 3 queues in flight, churn) for 120 s, four stress processes at once,
#     20 draws in 20 new processes (each a new VMID context with the known harmless fault), and compute again at the end.
# All passed: /Users/Shared/gpu/ws-ready is written, and `sudo bash install_boot.sh --ws-trial` accepts it.
# Nothing else is installed or changed. Run: bash ~/gpu/try15.sh  ->  send ~/nullarc_try15.txt
LOG=~/nullarc_try15.txt
G=/Users/Shared/gpu
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"; rm -f ~/nullarc_t15.*
START=$(date +%s)
say() { echo "$1"; echo "$1" >> "$LOG"; sync; }
run() { "$@" 2>&1 | while IFS= read -r l; do say "$l"; done; }
FAILS=""; WARNS=""
bad()  { say "   FAIL: $1"; FAILS="$FAILS
  - $1"; }
warn() { say "   warning: $1"; WARNS="$WARNS
  - $1"; }
ok()   { say "   ok: $1"; }
# timed <seconds> <tag> <cmd...>: run with a time limit, log its output, leave its exit code in $RC (124: killed).
# A process stuck in the kernel on a hung GPU does not die from kill -9: it is left behind, never waited for.
timed() {
    local lim=$1 tag=$2 out=~/nullarc_t15.$2.out; shift 2
    "$@" > "$out" 2>&1 & local p=$! t=0
    while kill -0 $p 2>/dev/null && [ $t -lt $lim ]; do sleep 1; t=$((t+1)); done
    if kill -0 $p 2>/dev/null; then
        kill -9 $p 2>/dev/null; RC=124
        while IFS= read -r l; do say "$l"; done < "$out"
        say "!! $tag still running after ${lim}s: killed (GPU hang?)"
    else
        wait $p; RC=$?
        while IFS= read -r l; do say "$l"; done < "$out"
        say "rc=$RC"
    fi
}
# the mtlprobe next to this script (run from the package: gpu/../metal), else the installed one; it must know `stress`
HERE=$(cd "$(dirname "$0")" && pwd)
MP=~/gpu/metal/mtlprobe; [ -x "$HERE/../metal/mtlprobe" ] && MP=$(cd "$HERE/../metal" && pwd)/mtlprobe
reg() { sudo ~/gpu/phxctl/phxctl regs "$@" 2>/dev/null | awk '{print $NF}'; }

say "== $(date '+%F %T') $(sw_vers -productVersion) try15"
[ "$(ioreg -l | sed -n 's/.*"phx-chip" = "\(.*\)".*/\1/p' | head -1)" = strix-point ] || { say "STOP: PhxBringup/Strix not up"; exit 1; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || {
    say "STOP: NMXAccel does not run: the GPU was not brought up at boot. Check: tail -30 /var/log/phxboot.log"
    say "      (the LaunchDaemon: sudo bash install_boot.sh; turned off by $G/phxboot-off?)"; exit 1; }
[ -x "$MP" ] || { say "STOP: no $MP: run 'bash install.sh' from the package first"; exit 1; }
grep -a -q 'stress: FAIL' "$MP" || { say "STOP: $MP is an old mtlprobe without the stress test: run 'bash install.sh' from nullarc-strix-19"; exit 1; }
say "   mtlprobe: $MP"
sudo -v || exit 1

say "== 1. boot path"
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
if [ "${B:-0}" -ge 33 ]; then ok "PhxBringup build $B"
else bad "PhxBringup build ${B:-?}: the EFI this Mac booted from has an old kext (33 is on the USB stick's EFI)"; fi
[ -f /Library/LaunchDaemons/local.phxboot.plist ] && ok "LaunchDaemon local.phxboot installed" || bad "no LaunchDaemon: sudo bash install_boot.sh"
[ -e $G/phxboot-off ] && bad "kill switch $G/phxboot-off present (an earlier boot did not finish the bring-up?)"
BOOT=$(sysctl -n kern.boottime | sed -n 's/.*sec = \([0-9]*\),.*/\1/p')
HDR=$(sudo grep -n '^== [0-9-]* [0-9:]* phxboot' /var/log/phxboot.log 2>/dev/null | tail -1)
HN=${HDR%%:*}; HT=$(echo "$HDR" | sed -n 's/^[0-9]*:== \([0-9-]* [0-9:]*\) phxboot.*/\1/p')
HS=$(date -j -f '%Y-%m-%d %H:%M:%S' "$HT" +%s 2>/dev/null)
say "   phxboot.log, this boot's run:"
[ -n "$HN" ] && sudo tail -n +"$HN" /var/log/phxboot.log | head -40 | while IFS= read -r l; do say "     | $l"; done
if [ -z "$HS" ] || [ "$HS" -lt $((BOOT - 5)) ]; then bad "phxboot did not run on this boot (last run: ${HT:-never})"
elif sudo tail -n +"$HN" /var/log/phxboot.log | grep -q '^done '; then ok "phxboot ran at $HT and finished"
else bad "phxboot's run at $HT did not reach 'done'"; fi
ioreg -rc NMXAccel -d1 | grep -q '"MetalPluginName"' && ok "Metal plugin name on NMXAccel" || bad "MetalPluginName not on NMXAccel (headless check pending?)"
say "   allow list: $(grep -v '^#' /Library/GPUBundles/nvmtl-allow.txt 2>/dev/null | tr '\n' ' ')($(head -1 /Library/GPUBundles/nvmtl-allow.txt 2>/dev/null))"
FC=$(reg 0x2828)
[ "$FC" = 0x3ffffffc ] && ok "GCVM fault redirect on (FAULT_CNTL $FC)" || bad "GCVM_L2_PROTECTION_FAULT_CNTL = ${FC:-?}, want 0x3ffffffc: without the redirect the first IB hangs the gfx queue"
run sudo ~/gpu/phxctl/mesboot gpustat

say "== 2. display (read only)"
system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Chipset|Resolution|Main Display|Online|Connection|Display Type|Framebuffer' |
    sed 's/^ */   /' | while IFS= read -r l; do say "$l"; done
FBC=$(ioreg -rc IOFramebuffer -d0 | sed -n 's/.*<class \([A-Za-z0-9_]*\),.*/\1/p' | sort | uniq -c | tr '\n' ' ')
say "   framebuffers now: ${FBC:-none}"
ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && warn "PhxAMDFB already has the panel on this boot (an armed boot?)"
O=$(reg 0x4fea 0x4fef 0x5003 0x500d); set -- $O 0 0 0 0
HT1=$(( ($1 & 0x7fff) + 1 )); VT1=$(( ($2 & 0x7fff) + 1 )); CTL=$3; F0=$(( $4 & 0xffffff ))
T0=$(perl -MTime::HiRes=time -e 'printf "%.6f", time'); sleep 2
F1=$(reg 0x500d); F1=$(( ${F1:-0} & 0xffffff )); T1=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
FR=$(( (F1 - F0) & 0xffffff ))
HZ=$(echo "$FR $T0 $T1" | awk '{ if ($3 > $2) printf "%.2f", $1 / ($3 - $2); else print 0 }')
say "   OTG0: control $CTL, totals ${HT1}x${VT1}, $FR frames in $(echo "$T0 $T1" | awk '{printf "%.2f", $2 - $1}') s -> $HZ Hz"
if [ $(( CTL & 1 )) = 1 ] && [ "$FR" -ge 8 ] && [ "$HT1" -ge 100 ] && [ "$VT1" -ge 100 ]; then
    ok "OTG0 runs on the DCN 3.5 offsets PhxAMDFB reads (it will measure ~$HZ Hz)"
else
    warn "OTG0 does not look like a running panel timing: PhxAMDFB will assume 60 Hz (the panel still works)"
fi
nvram phxfb-boot >/dev/null 2>&1 && warn "NVRAM phxfb-boot is set: the next boot will try WindowServer on the GPU"
AL=$(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null)
FV=$(fdesetup status 2>/dev/null | head -1)
if [ -n "$AL" ]; then ok "automatic login: $AL"
else warn "automatic login is off ($FV): on the GPU boot the login window may stay black; turn it on in System Settings -> Users & Groups before --ws-trial"; fi

say "== 3. Metal"
say "-- mtlprobe list"; timed 45 list $MP list
say "-- compute"; timed 60 compute $MP
[ $RC = 0 ] && grep -q 'results: 4096/4096' ~/nullarc_t15.compute.out && ok "compute 4096/4096" || bad "Metal compute (rc $RC)"
say "-- draw"; timed 60 draw $MP render
[ $RC = 0 ] && ok "draw" || bad "Metal draw (rc $RC)"
say "-- stress, one process, 120 s"
timed 300 stress1 $MP stress 120
WHY=$(grep 'stress: FAIL' ~/nullarc_t15.stress1.out | head -1); [ $RC = 124 ] && WHY="hung in step '$(tail -1 ~/nullarc_t15.stress1.out | awk '{print $NF}')', killed after 300 s"
grep -q 'all correct' ~/nullarc_t15.stress1.out || { [ $RC = 0 ] && RC=99 && WHY="no 'all correct' line"; }
[ $RC = 0 ] && ok "stress 120 s: $(grep -o '[0-9]* iterations' ~/nullarc_t15.stress1.out)" || bad "stress (rc $RC): $WHY"
if [ $RC = 124 ]; then
    say "!! the GPU looks hung: the rest of the Metal tests are skipped"
else
    say "-- stress, four processes at once, 30 s each"
    pids=""
    for n in 1 2 3 4; do ( $MP stress 30 > ~/nullarc_t15.par$n.out 2>&1; echo $? > ~/nullarc_t15.par$n.rc ) & pids="$pids $!"; done
    alive() { for p in $pids; do kill -0 $p 2>/dev/null && return 0; done; return 1; }
    t=0; while [ $t -lt 240 ] && alive; do sleep 2; t=$((t+2)); done
    for n in 1 2 3 4; do
        r=$(cat ~/nullarc_t15.par$n.rc 2>/dev/null || echo 124)
        say "   process $n: rc=$r $(tail -1 ~/nullarc_t15.par$n.out)"
        [ "$r" = 0 ] && ! grep -q 'all correct' ~/nullarc_t15.par$n.out && r=99
        [ "$r" = 0 ] || bad "parallel stress process $n (rc $r): $(grep 'stress: FAIL' ~/nullarc_t15.par$n.out | head -1)"
    done
    alive && { pkill -9 -f "$MP stress" 2>/dev/null; bad "parallel stress did not finish in 240 s"; }
    say "-- 20 draws, each in a new process (a new VMID context each)"
    good=0
    for n in $(seq 1 20); do
        timed 45 seq $MP render > /dev/null
        [ $RC = 0 ] && good=$((good + 1))
        [ $RC = 124 ] && break
    done
    [ $good = 20 ] && ok "20 of 20 draws in new processes" || bad "$good of 20 draws in new processes"
    say "-- compute once more (the queues still run)"; timed 60 compute2 $MP
    [ $RC = 0 ] && ok "compute after the load" || bad "compute after the load (rc $RC)"
fi
run sudo ~/gpu/phxctl/mesboot gpustat
say "-- kernel: PhxBringup / PhxAccel complaints during the run"
log show --last "$(( $(date +%s) - START + 30 ))s" --style compact \
    --predicate 'sender == "PhxBringup" OR sender == "PhxAccel"' 2>/dev/null |
    grep -i -E 'hang|timeout|timed out|fail|error|reset|stuck' | grep -v 'cfg+0x' | tail -20 | while IFS= read -r l; do say "   $l"; done

say "== summary"
if [ -z "$FAILS" ]; then
    { echo "$(date '+%F %T') try15 passed: build $B, $HZ Hz panel, stress ok"; } | sudo tee $G/ws-ready > /dev/null
    say "READY for WindowServer on the GPU: everything passed${WARNS:+ (warnings:$WARNS)}"
    say "next: sudo bash install_boot.sh --ws-trial   (one boot on the GPU; the boot after it is normal again)"
else
    sudo rm -f $G/ws-ready
    say "NOT READY:$FAILS"
    [ -n "$WARNS" ] && say "warnings:$WARNS"
fi
for v in /Volumes/*; do [ -e "$v/nullarc-strix-19.zip" ] && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
