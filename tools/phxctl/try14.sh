#!/bin/bash
# Strix Point, step 14: the boot path and drawing.
#  1. The GPU comes up exactly as phxboot will run it at boot (full.sh kiqsched with phxboot's options, no test flags):
#     mesboot now turns the fault redirect on by itself on Strix, so the first IB must not hang (try7) without it.
#  2. Metal compute once, then a draw (mtlprobe render: one triangle, read back) in three separate processes - each a
#     new VMID context, where the left-over CP read happens: if it cost a context its state, a draw would show it.
# Nothing is installed. Run: bash ~/gpu/try14.sh  ->  send ~/nullarc_try14.txt
LOG=~/nullarc_try14.txt
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; sync; }
run() { "$@" 2>&1 | while IFS= read -r l; do say "$l"; done; }
timed() {   # timed <seconds> <cmd...>: run with a time limit, log everything
    local lim=$1; shift
    ( "$@" > ~/nullarc_t14.out 2>&1 & p=$!; t=0
      while kill -0 $p 2>/dev/null && [ $t -lt $lim ]; do sleep 1; t=$((t+1)); done
      if kill -0 $p 2>/dev/null; then echo "!! still running after ${lim}s: killed (GPU hang?)"; kill -9 $p; else wait $p; echo "rc=$?"; fi
    ) > ~/nullarc_t14.rc 2>&1
    while IFS= read -r l; do say "$l"; done < ~/nullarc_t14.out
    while IFS= read -r l; do say "$l"; done < ~/nullarc_t14.rc
}
say "== $(date '+%F %T') $(sw_vers -productVersion)"
[ "$(ioreg -l | sed -n 's/.*"phx-chip" = "\(.*\)".*/\1/p' | head -1)" = strix-point ] || { say "STOP: PhxBringup/Strix not up"; exit 1; }
sudo -v || exit 1
say "== PhxBringup build $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)"
say "== bring-up as phxboot runs it: MC=1 CS=1 ADOPT=1 GFX=1 AG=1 full.sh kiqsched"
MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash ~/gpu/full.sh kiqsched 2>&1 | while IFS= read -r l; do say "$l"; done
grep -E 'TMR|STOP|WARNING|IC base' ~/gpu/bringup.log 2>/dev/null | while IFS= read -r l; do say "   bringup: $l"; done
sudo bash -c 'nohup shutdown -r +6 >/dev/null 2>&1 &'
say "== gpu state"
run sudo ~/gpu/phxctl/mesboot gpustat
say "== claim: the Metal accelerator starts on the GPU"
run sudo ~/gpu/phxctl/mesboot claim
for i in $(seq 1 20); do ioreg -rc NMXAccel -d0 | grep -q NMXAccel && break; sleep 1; done
say "== waiting for the headless check (up to 75 s)"
for i in $(seq 1 75); do ioreg -rc NMXAccel -d1 | grep -q '"MetalPluginName"' && break; sleep 1; done
say "== Metal compute"
timed 45 ~/gpu/metal/mtlprobe
for n in 1 2 3; do
    say "== Metal draw #$n (a new process = a new VMID context)"
    timed 45 ~/gpu/metal/mtlprobe render
done
say "== gpu state after the draws"
run sudo ~/gpu/phxctl/mesboot gpustat
OK=$(grep -c '^pixels: red ' "$LOG"); GOOD=$(grep -A1 '^pixels: red ' "$LOG" | grep -c '^rc=0')
say "== summary: redirect $(grep -q 'fault redirect on' "$LOG" && echo on by default || echo NOT ON), compute $(grep -q 'results: 4096/4096' "$LOG" && echo OK || echo FAIL), draws $GOOD of 3 correct ($OK finished)"
sudo killall shutdown 2>/dev/null
say "== done $(date '+%T'), watchdog cancelled. Send $LOG"
