#!/bin/bash
# vcn_test.sh: the 880M's video engine (VCN 4.0.5) brought up by hand (phase 2.1) and fed by a client (2.2, PhxBringup
# build 42+). Nothing at boot changes: phxboot and the plugin stay as they are, PhxBringup 42 without `vcn adopt` behaves
# as 41, and the next boot is as before whatever happens here.
#   1. state of MMHUB and VCN (read-only)
#   2. mesboot vcn up: MMHUB for VCN (once), the VCN firmware through the PSP (once per boot), PowerUpVcn0, the DPG SRAM
#      image through the PSP, the unified ring in VRAM
#   3. mesboot vcn ringtest: END packets on the ring, the read pointer must move
#   4. mesboot vcn adopt: the ring goes to PhxBringup; vcnjob (an ordinary client) sends VCN jobs in its own VMID
#      (MMHUB): each writes a value into its buffer, checked on the CPU; then release and down
#   5. vkvideo (2.3): this round's RADV (Mesa 0009, H.264/H.265 decode built in), opened from the round folder - the
#      installed one WindowServer uses stays - must show a video decode queue, H.264/H.265 capabilities and bring up a
#      decode session, as Linux's RADV does on this 880M; the installed RADV must still show none
#   6. the whole of it once more (Linux starts and stops VCN this way at every job)
# A reboot is armed for 5 minutes and cancelled at the end: if the display or the machine stops, it comes back by itself.
# Run: bash gpu/vcn_test.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-vcn)
set -u
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu
LOG=~/nullarc_vcn.txt; : > "$LOG"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
# this round's mesboot (with `vcn`) and the firmware sit in gpu/vcn/, apart from gpu/phxctl: the boot daemon's mesboot
# stays the one it runs. Copied off the stick first (exec bit, no quarantine).
T=$(mktemp -d /tmp/vcn.XXXXXX)
cp "$HERE/vcn/mesboot" "$HERE/vcn/vcn_4_0_5.bin" "$HERE/vcn/vcnjob" "$HERE/vcn/vkvideo" "$HERE/vcn/libvulkan_radeon.dylib" "$T/" 2>/dev/null
chmod +x "$T/mesboot" "$T/vcnjob" "$T/vkvideo" 2>/dev/null
xattr -d com.apple.quarantine "$T/mesboot" "$T/vcnjob" "$T/vkvideo" "$T/libvulkan_radeon.dylib" 2>/dev/null
codesign --force --sign - "$T/libvulkan_radeon.dylib" >/dev/null 2>&1
MB=$T/mesboot; FW=$T/vcn_4_0_5.bin
say() { echo "$*" | tee -a "$LOG"; }
finish() {
    sudo -n killall shutdown 2>/dev/null && say "== the armed reboot is cancelled"
    sudo -n dmesg 2>/dev/null | tail -60 > ~/nullarc_vcn_dmesg.txt
    bash "$LOGS" vcn "$LOG" ~/nullarc_vcn_dmesg.txt | tail -1 | tee -a "$LOG"
}
trap finish EXIT
sudo -v || exit 1
say "== vcn_test $(date '+%F %T'), $(sw_vers -productVersion), PhxBringup $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot (phxboot)"; exit 1; }
[ -x "$MB" ] && [ -f "$FW" ] || { say "STOP: no mesboot or firmware in $HERE/vcn"; exit 1; }
say "firmware $FW ($(stat -f %z "$FW") bytes), mesboot $MB"
M() { say "\$ mesboot $*"; sudo "$MB" "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }
J() { say "\$ vcnjob $*"; "$T/vcnjob" "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }
# vkvideo with a time limit: a stuck driver call must not hold the round (perl alarm, as t48.sh did for vkprobe)
VV() { say "\$ vkvideo $*"; perl -e 'alarm 60; exec @ARGV' "$T/vkvideo" "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }
cycle() {   # up, ring test, adopt, client jobs, release, down; rc 0 when every step passed
    local rc=0
    M vcn up "$FW" || { M vcn down; return 1; }
    M vcn ringtest 3 || rc=2
    if [ $rc = 0 ]; then
        if M vcn adopt "$FW"; then
            J 16 || rc=3
            VV "$T/libvulkan_radeon.dylib" || rc=5
            say "(the installed RADV, WindowServer's: no decode queue expected)"
            VV /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib
            M vcn release || { sleep 2; M vcn release; }
        else rc=4; fi
    fi
    M vcn down
    return $rc
}
sudo bash -c 'nohup shutdown -r +5 >/dev/null 2>&1 &'; say "== reboot armed for 5 minutes (cancelled at the end)"
sync
M vcn status
cycle; rc=$?
say "== first cycle: rc $rc"
if [ $rc = 0 ]; then
    say "== second cycle (as Linux does at every job)"
    cycle; say "== second cycle: rc $?"
fi
M vcn status
say "MMHUB flushes without the semaphore: $(sysctl -n debug.phxvcn_sem_miss 2>/dev/null), not acked: $(sysctl -n debug.phxvcn_ack_miss 2>/dev/null)"
say "== PhxBringup's kernel lines:"
sudo -n dmesg 2>/dev/null | grep -E 'PhxBringup: (adopted|VCN|MMHUB)' | tail -12 | tee -a "$LOG"
pgrep -x WindowServer >/dev/null && say "WindowServer still runs (pid $(pgrep -x WindowServer))" || say "WindowServer GONE"
say "== done $(date '+%T')"
