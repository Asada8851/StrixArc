#!/bin/bash
# Metal only, in a boot where phxboot brought the GPU up (bring-up + claim done). Log: ~/nullarc_mtl.txt
OUT=~/nullarc_mtl.txt; exec > >(tee "$OUT") 2>&1
echo "== $(date '+%F %T')"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { echo "NMXAccel is not running: the GPU was not brought up at boot (tail /var/log/phxboot.log)"; exit 1; }
for i in $(seq 1 75); do ioreg -rc NMXAccel -d1 | grep -q '"MetalPluginName"' && break; sleep 1; done
ioreg -rc NMXAccel -d1 | grep -E '"MetalPluginName"|Headless'; sysctl debug.nmxaccel_headless
echo "== mtlprobe"; . ~/gpu/mtlrun.sh; mtlprobe_limited
echo "== done. Send $OUT"
