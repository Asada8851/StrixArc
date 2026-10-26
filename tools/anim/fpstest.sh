#!/bin/bash
# fpstest.sh <seconds> <label>: run the synthetic animation in the console user's session and report fps / copy time
S=${1:-12}; L=${2:-run}
U=$(stat -f %u /dev/console); N=$(stat -f %Su /dev/console)
[ "$N" = root ] && { echo "$L: nobody logged in"; exit 1; }
(sudo launchctl asuser $U sudo -u $N /Users/dripside/gpu/anim/anim $((S + 5)) >/tmp/anim.log 2>&1 &)
sleep 3
a=$(sysctl -n debug.nvaccel_iop_ok); s=$(sysctl -n debug.nvaccel_iop_copy_us_sum); t0=$(ps -o time= -p $(pgrep -x WindowServer))
sudo sysctl -w debug.nvaccel_iop_copy_us_max=0 >/dev/null 2>&1
sleep $S
b=$(sysctl -n debug.nvaccel_iop_ok); e=$(sysctl -n debug.nvaccel_iop_copy_us_sum); t1=$(ps -o time= -p $(pgrep -x WindowServer))
n=$((b - a)); [ $n -gt 0 ] || n=1
echo "$L: $(( (b - a) / S )) fps ($((b - a)) frames in ${S}s), copy avg $(( (e - s) / n )) us, max $(sysctl -n debug.nvaccel_iop_copy_us_max) us, WS cpu $t0 -> $t1"
sleep 6
