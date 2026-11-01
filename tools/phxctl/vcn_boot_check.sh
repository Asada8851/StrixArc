#!/bin/bash
# vcn_boot_check.sh: after the boot that installed the new RADV and brought VCN up by itself (round 60), what everyday use
# gets, with no test script holding VCN:
#   1. RADV: the installed one is the package's, phxboot kept it (WindowServer got through its watch on it)
#   2. VCN: up since boot and held by PhxBringup (phx-vcn), the boot's log, the job counter (debug.phxvcn_jobs, build 44)
#   3. Metal programs on the new RADV (mtlprobe compute, render, stress 15 s)
#   4. the everyday player: vplay plays 4K HEVC, 4K H.264 and 20 s of the clip with sound - as VCN Player.app does for
#      Finder - each with its vplay.log line, the VCN jobs it made and its CPU time
#   5. what VCN held costs at idle: MMHUB flushes and WindowServer's CPU over 20 s of an idle desktop
# Run: bash gpu/vcn_boot_check.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-vcnboot)
set -u
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu; VID=$HERE/../video
LOG=~/nullarc_vcn_boot.txt; : > "$LOG"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
RADV=/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib
T=$(mktemp -d /tmp/vcnboot.XXXXXX)
say() { echo "$*" | tee -a "$LOG"; }
finish() {
    cp ~/Library/Logs/vplay.log "$T/vplay.log" 2>/dev/null; cp $G/logs/vcn-boot.txt "$T/vcn-boot.log" 2>/dev/null
    bash "$LOGS" vcnboot "$LOG" "$T"/*.log | tail -1 | tee -a "$LOG"; rm -rf "$T"
}
trap finish EXIT
lim() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }
jobs() { sysctl -n debug.phxvcn_jobs 2>/dev/null || echo -1; }
cputime() { ps -o time= -p "$1" 2>/dev/null | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%.2f", s }'; }
say "== vcn_boot_check $(date '+%F %T'), $(sw_vers -productVersion), PhxBringup $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot (phxboot)"; exit 1; }

say "== 1. RADV"
want=$(shasum "$HERE/nvmtl/libvulkan_radeon.dylib" 2>/dev/null | cut -c1-12)
say "   installed $(wc -c < "$RADV" | tr -d ' ') bytes; package's $want; $( [ -e $G/radv.pending ] && echo "STILL ON TRIAL ($(head -1 $G/radv.pending))" || echo 'not on trial (kept)')"
say "   (the installed copy is re-signed, so its checksum differs from the package's; the backup: $(ls -td /Users/Shared/nullarc-backup/radv-update.* 2>/dev/null | head -1))"
grep -E 'RADV|VCN|WindowServer pid|^== ' /var/log/phxboot.log | tail -8 | sed 's/^/   phxboot: /' | tee -a "$LOG"

say "== 2. VCN"
held=no; ioreg -rc PhxBringup -d1 | grep -q '"phx-vcn" = 1' && held=yes
say "   held by PhxBringup since boot: $held; VCN jobs so far: $(jobs)"
[ -f $G/logs/vcn-boot.txt ] && tail -4 $G/logs/vcn-boot.txt | sed 's/^/   vcn-boot: /' | tee -a "$LOG"
[ $held = yes ] || say "   !! VCN is not up: vplay will decode on the CPU (phxboot-vcn: $([ -e $G/phxboot-vcn ] && echo set || echo 'not set'))"

say "== 3. Metal programs on the installed RADV"
MP=~/gpu/metal/mtlprobe
for t in "" render "stress 15"; do
    lim 60 "$MP" $t > "$T/mtl.out" 2>&1; rc=$?
    say "   mtlprobe ${t:-compute}: rc $rc  $(grep -E 'results:|pixels:|stress\[[0-9]+\].*(FAIL|ok)|FAIL' "$T/mtl.out" | tail -1 | cut -c1-110)"
    cat "$T/mtl.out" >> "$T/mtlprobe.log"
done

say "== 4. vplay (what VCN Player.app runs for Finder)"
if [ -x /usr/local/bin/vplay ] && [ -d "/Applications/VCN Player.app" ]; then
    say "   /usr/local/bin/vplay and /Applications/VCN Player.app installed"
    for c in "hevc_2160" "h264_2160" "user_av -t 20"; do
        set -- $c; f=$VID/$1.mp4; shift
        [ -f "$f" ] || { say "   no $f"; continue; }
        j0=$(jobs)
        lim 90 /usr/bin/time -p /usr/local/bin/vplay "$f" -autoexit -x 1600 "$@" > /dev/null 2> "$T/vplay-time.txt"; rc=$?
        cpu=$(awk '/^(user|sys) /{s+=$2} END {printf "%.2f", s}' "$T/vplay-time.txt"); real=$(awk '/^real /{print $2}' "$T/vplay-time.txt")
        say "   $(basename "$f"): exit $rc, VCN jobs +$(( $(jobs) - j0 )), CPU ${cpu}s over ${real}s"
        say "      vplay.log: $(tail -1 ~/Library/Logs/vplay.log 2>/dev/null)"
        [ $rc = 142 ] && { say "   !! vplay hit its time limit"; break; }
    done
else say "   !! the player is not installed (go.sh installs it: gpu/install_player.sh)"; fi

say "== 5. idle, 20 s, VCN $held"
WS=$(pgrep -x WindowServer | head -1)
f0=$(sysctl -n debug.phxvcn_sem_miss 2>/dev/null || echo 0); w0=$(cputime "$WS"); sleep 20
f1=$(sysctl -n debug.phxvcn_sem_miss 2>/dev/null || echo 0); w1=$(cputime "$WS")
say "   MMHUB flushes (mirrored GPU mappings) $((f1 - f0)), WindowServer CPU +$(awk -v a=${w0:-0} -v b=${w1:-0} 'BEGIN { printf "%.2f", b - a }')s"
pgrep -x WindowServer >/dev/null && say "WindowServer still runs" || say "WindowServer GONE"
say "== done $(date '+%T')"
