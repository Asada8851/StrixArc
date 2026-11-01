#!/bin/bash
# Strix Point (880M) video decoder: what amdgpu does to power up VCN 4.0.5, start it and feed it, for StrixArc's
# hardware decode under macOS (phase 2). Runs in the normal Linux desktop: no reboot, no blacklist. As your user, not
# root; it asks for the sudo password once (tracefs, debugfs).
#
# VCN sits power-gated until a job comes; amdgpu then powers it up through the SMU, has the PSP load the DPG SRAM
# image (indirect register writes), starts the ring, and gates it again ~1 s after the last job. So each phase is a
# short decode with RADV's Vulkan Video (the same decode path StrixArc ports) followed by idle time:
#   1. idle, VCN gated              2. H.264 1080p, 60 frames (start)   3. 3 s idle (stop)
#   4. HEVC 4K, 60 frames (start)   5. 3 s idle (stop)                  6. VA-API (radeonsi) H.264, for comparison
# With VCN_REGS=1, during a longer decode the VCN, MMHUB and NBIO registers listed in vcn_regs_strix.txt are read once
# through debugfs: the state a working driver leaves while VCN runs (ring, cache windows, VM contexts, apertures).
# The list leaves out registers whose READ changes state: MMVM_INVALIDATE_ENGn_SEM acquires the hub's invalidation
# semaphore (the first version read ENG0_SEM, took it for good, and VCN's next TLB flush waited forever: ring timeouts
# and resets in a loop, the desktop frozen, 2026-10-27), and UVD_DPG_LMA_* drive indirect register access.
#
# Recorded: every amdgpu register write and read (amdgpu_device_wreg/rreg) and kprobes on SMU messages, PSP commands
# (cmd id, address, size, firmware type of LOAD_IP_FW), VCN start/stop, the SRAM update, ring begin/end use.
# Output: ~/strix_vcn_trace.txt.zst, ~/strix_vcn_regs.txt, ~/strix_vcn_state.txt, ~/strix_vcn_dmesg.txt
set -u
[ "$(id -u)" != 0 ] || { echo "run it as your user, not with sudo (it asks for the password itself)"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
T=/sys/kernel/tracing
OUT=~/strix_vcn; STATE=$OUT"_state.txt"; REGS=$OUT"_regs.txt"
CLIPS=${CLIPS:-$HOME/opt/nullarc-cross/clips}
H264=$CLIPS/h264_1080.mp4; HEVC=$CLIPS/hevc_2160.mp4
R() { sudo tee "$1" > /dev/null; }   # echo VALUE | R FILE: a root write
for f in "$H264" "$HEVC" "$HERE/vcn_regs_strix.txt"; do [ -f "$f" ] || { echo "STOP: missing $f"; exit 1; }; done
command -v ffmpeg > /dev/null || { echo "STOP: no ffmpeg"; exit 1; }
sudo -v || exit 1
[ -d $T/events/amdgpu ] || sudo mount -t tracefs nodev $T 2>/dev/null
sudo test -d $T/events/amdgpu || { echo "STOP: no amdgpu trace events in $T"; exit 1; }
# the 880M's debugfs node: dri/N/name reads "amdgpu dev=0000:64:00.0 ...", or newer kernels name the directory by the
# PCI address. /sys/kernel/debug is root's only, so the search itself runs as root.
sudo test -d /sys/kernel/debug/dri || sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
DBG=$(sudo sh -c 'for d in /sys/kernel/debug/dri/*; do
    [ -e "$d/amdgpu_regs" ] || continue
    case "$d" in *0000:64:00.0) echo "$d"; exit ;; esac
    grep -qs "0000:64:00.0" "$d/name" && { echo "$d"; exit; }
done')
if [ -z "$DBG" ]; then
    echo "STOP: no debugfs dri node with amdgpu_regs for 0000:64:00.0; what there is:"
    sudo sh -c 'for d in /sys/kernel/debug/dri/*; do echo "  $d: $(cat "$d/name" 2>/dev/null) $(ls "$d" | grep -c amdgpu_) amdgpu_* files"; done'
    exit 1
fi
echo "debugfs: $DBG"

BUF0=$(sudo cat $T/buffer_size_kb | sed -n 's/.*expanded: \([0-9]*\).*/\1/p;t;s/^\([0-9]*\).*/\1/p'); BUF0=${BUF0:-1408}
DMESGP=""
cleanup() {
    echo 0 | R $T/tracing_on
    for e in amdgpu/amdgpu_device_wreg amdgpu/amdgpu_device_rreg nullarc; do
        sudo test -e $T/events/$e/enable && echo 0 | R $T/events/$e/enable
    done
    sudo sh -c "grep -q nullarc $T/kprobe_events && echo > $T/kprobe_events" 2>/dev/null
    echo "$BUF0" | R $T/buffer_size_kb
    [ -n "$DMESGP" ] && sudo kill "$DMESGP" 2>/dev/null
}
trap cleanup EXIT INT TERM

echo "== trace setup"
echo 0 | R $T/tracing_on
echo 65536 | R $T/buffer_size_kb
echo 0 | R $T/options/overwrite
echo | R $T/trace
echo 1 | R $T/events/amdgpu/amdgpu_device_wreg/enable
echo 1 | R $T/events/amdgpu/amdgpu_device_rreg/enable
# kprobes: x86-64 arguments in %di, %si, %dx, %cx; each one is optional (an inlined or renamed function is skipped)
kp() { echo "$1" | sudo tee -a $T/kprobe_events > /dev/null 2>&1 && echo "   kprobe ${1%% *}" || echo "   (no kprobe for: ${1#* })"; }
kp "p:nullarc/smu_msg smu_cmn_send_smc_msg_with_param msg=%si:u32 param=%dx:u32"
# psp_cmd_submit_buf(psp, ucode, cmd, fence): cmd_id at +8, LOAD_IP_FW's address/size/type at +28..+40
kp "p:nullarc/psp_cmd psp_cmd_submit_buf cmd_id=+8(%dx):u32 lo=+28(%dx):u32 hi=+32(%dx):u32 size=+36(%dx):u32 type=+40(%dx):u32"
kp "p:nullarc/vcn_start vcn_v4_0_5_start"
kp "p:nullarc/vcn_stop vcn_v4_0_5_stop"
kp "p:nullarc/vcn_sram amdgpu_vcn_psp_update_sram inst=%si:s32 ucode_id=%dx:s32"
kp "p:nullarc/vcn_pg vcn_v4_0_5_set_pg_state state=%si:u32"
kp "p:nullarc/ring_begin amdgpu_vcn_ring_begin_use"
kp "p:nullarc/ring_end amdgpu_vcn_ring_end_use"
kp "p:nullarc/vcn_idle amdgpu_vcn_idle_work_handler"
sudo test -d $T/events/nullarc && echo 1 | R $T/events/nullarc/enable
sudo rm -f "$OUT""_dmesg.txt"; sudo sh -c "dmesg -W > '$OUT""_dmesg.txt' 2>&1" & DMESGP=$!

phase() { echo "nullarc phase $1" | R $T/trace_marker; echo "== $1"; }
dec() { timeout 60 ffmpeg -hide_banner -v error -hwaccel vulkan -i "$1" -frames:v "$2" -f null - < /dev/null; }
echo 1 | R $T/tracing_on
phase "1: idle (VCN gated)";            sleep 2
phase "2: vulkan h264 1080p 60 frames"; dec "$H264" 60
phase "3: idle 3 s";                    sleep 3
phase "4: vulkan hevc 2160p 60 frames"; dec "$HEVC" 60
phase "5: idle 3 s";                    sleep 3
phase "6: vaapi h264 1080p 60 frames"
timeout 60 ffmpeg -hide_banner -v error -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i "$H264" -frames:v 60 -f null - < /dev/null
phase "7: idle 3 s";                    sleep 3
phase "end"
echo 0 | R $T/tracing_on

# registers while VCN runs: a full HEVC decode in the background, read 1 s in (outside the trace: a read is not a
# write, and the trace would only fill up with them). Off by default: ~/strix_vcn_regs.txt from the first run is kept.
if [ "${VCN_REGS:-0}" = 1 ]; then
echo "== registers while VCN decodes"
grep -qE 'INVALIDATE_ENG|_SEM\b|UVD_DPG_' "$HERE/vcn_regs_strix.txt" && { echo "STOP: the register list has side-effect reads"; exit 1; }
timeout 60 ffmpeg -hide_banner -v error -hwaccel vulkan -stream_loop 3 -i "$HEVC" -f null - < /dev/null & FP=$!
sleep 1
sudo python3 -I - "$DBG/amdgpu_regs" "$HERE/vcn_regs_strix.txt" > "$REGS" <<'EOF'
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
for line in open(sys.argv[2]):
    ip, name, a = line.split()
    try:
        v = int.from_bytes(os.pread(fd, 4, int(a, 16) * 4), 'little')
        print(f"{ip:6} {name:48} {a:>9} 0x{v:08x}")
    except OSError as e:
        print(f"{ip:6} {name:48} {a:>9} error {e.errno}")
EOF
wait $FP
# the VCN doorbell range is a PCIe-port register (regGDC0_BIF_VCN0_DOORBELL_RANGE): through amdgpu_regs_pcie if there
sudo python3 -I - "$DBG/amdgpu_regs_pcie" >> "$REGS" <<'EOF'
import os, sys
try:
    fd = os.open(sys.argv[1], os.O_RDONLY)
    for name, a in (("GDC0_BIF_VCN0_DOORBELL_RANGE", 0x10400 + 0x4f0af3), ("GDC0_BIF_SDMA0_DOORBELL_RANGE", 0x10400 + 0x4f0af0)):
        v = int.from_bytes(os.pread(fd, 4, a * 4), 'little')
        print(f"pcie   {name:48} {a:>#9x} 0x{v:08x}")
except OSError as e:
    print(f"pcie   (amdgpu_regs_pcie unreadable: errno {e.errno})")
EOF
fi

echo "== saving"
{ echo "# $(date '+%F %T') $(uname -r)"; echo "# $(ffmpeg -version | head -1)"
  echo "# Vulkan device:"; (vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo|apiVersion' | head -6)
  echo "# amdgpu firmware:"; sudo cat "$DBG/amdgpu_firmware_info" 2>/dev/null | grep -iE 'vcn|psp|smc|mes'
  echo "# VCN in dmesg:"; sudo dmesg | grep -iE 'vcn|jpeg' | tail -20
  echo "# kprobes"; sudo cat $T/kprobe_events; echo "# dropped events per CPU"; sudo sh -c "grep -H overrun $T/per_cpu/cpu*/stats" 2>/dev/null
} > "$STATE" 2>&1
sudo cat $T/trace | zstd -q -f -o "$OUT""_trace.txt.zst"
sleep 1; sudo chown "$(id -un)" "$OUT""_dmesg.txt" "$REGS" 2>/dev/null
Z() { zstd -dc "$OUT""_trace.txt.zst"; }
echo "lines: $(Z | wc -l), phases: $(Z | grep -c 'nullarc phase'), writes: $(Z | grep -c amdgpu_device_wreg), vcn starts: $(Z | grep -c 'vcn_start:'), psp commands: $(Z | grep -c 'psp_cmd:')"
echo "dropped: $(grep -E 'overrun' "$STATE" | awk '{s += $NF} END {print s + 0}') events (0 is what we want)"
[ "${VCN_REGS:-0}" = 1 ] && echo "registers read: $(grep -c 0x "$REGS")"
ls -la "$OUT"_trace.txt.zst "$OUT"_dmesg.txt "$STATE"
echo "done: tell Claude"
