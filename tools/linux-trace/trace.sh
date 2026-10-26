#!/bin/bash
# Run as root on a Linux live system (kernel >= 6.8, linux-firmware with gc_11_0_1/psp_13_0_4/sdma_6_0_1)
# booted on the Firebat with kernel cmdline: modprobe.blacklist=amdgpu   (so the driver is NOT loaded yet).
# Records every MMIO read/write amdgpu does while bringing up the 780M, with timestamps.
set -e
mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
T=/sys/kernel/tracing
echo 0 > $T/tracing_on
echo 262144 > $T/buffer_size_kb
: > $T/trace
echo 1 > $T/events/amdgpu/amdgpu_device_wreg/enable
echo 1 > $T/events/amdgpu/amdgpu_device_rreg/enable
echo 1 > $T/tracing_on
modprobe amdgpu ip_block_mask=0xffffffff dc=0 2>&1 | tail -3 || true   # dc=0: no display, keeps trace small
sleep 25
echo 0 > $T/tracing_on
cp $T/trace /tmp/amdgpu_regtrace.txt
dmesg | grep -iE "amdgpu|psp|rlc|mes|kiq" > /tmp/amdgpu_dmesg.txt
wc -l /tmp/amdgpu_regtrace.txt
echo "done: /tmp/amdgpu_regtrace.txt /tmp/amdgpu_dmesg.txt  (scp them to the Mac/PC)"
