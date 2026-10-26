#!/bin/bash
# Strix Point: can SDMA copy WindowServer's frames into the carve-out, and can the display flip there? (mesboot
# sdmatest; no kext change, no reboot.) The step before the kernel's frame copy moves from the compute queue (builds
# 34/35: a stuck copy took WindowServer with it) to an engine of its own. Run without sudo (it asks once):
#   bash gpu/sdma_test.sh      -> everything lands on the USB stick (nullarc_logs/<time>-sdma), also when it fails
# Best on a boot where WindowServer is on the firmware framebuffer (the state phxboot leaves after a failed
# WindowServer boot); it works with WindowServer on the GPU too. At the end the top of the panel shows five colour
# bands for 3 s: say whether you saw them.
LOG=~/nullarc_sdma.txt
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu
MB=~/gpu/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
finish() { say "== done $(date '+%T')"; bash "$LOGS" sdma "$LOG" | tee -a "$LOG"; }
trap finish EXIT
say "== $(date '+%F %T') $(sw_vers -productVersion) sdma_test"
sudo -v || exit 1
# keep sudo alive for the collector at the end (crash reports, panics)
( while sleep 50; do sudo -n true 2>/dev/null || exit; done ) & KEEP=$!
trap 'kill $KEEP 2>/dev/null; finish' EXIT

B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
say "PhxBringup build ${B:-not loaded}; phxboot switches: $(cd $G 2>/dev/null && ls -d phxboot-* 2>/dev/null | tr '\n' ' ')"
say "panel: $(ioreg -rc IOFramebuffer -d0 | sed -n 's/.*<class \([A-Za-z]*\).*/\1/p' | head -1); NMXAccel: $(ioreg -rc NMXAccel -d0 | grep -q NMXAccel && echo running || echo no)"
[ -n "$B" ] || { say "STOP: PhxBringup is not loaded (boot from the USB stick's EFI)"; exit 1; }
nvram boot-args 2>/dev/null | grep -q 'phxdebug=1' || { say "STOP: boot-args lack phxdebug=1 (mesboot needs the debug selectors)"; exit 1; }

# The GPU must be up (bring-up done this boot). phxboot normally does it; after a boot that did not finish, phxboot
# sets the kill switch and skips it from then on - then this script brings the GPU up itself, exactly as phxboot does,
# but only when nothing tried this boot (a second bring-up in one boot is refused by the PSP and can wedge the GPU).
if ! ioreg -rc NMXAccel -d0 | grep -q NMXAccel; then
    BOOT=$(sysctl -n kern.boottime | sed -n 's/^{ sec = \([0-9]*\),.*/\1/p')
    LAST=$(grep '^== .* phxboot$' /var/log/phxboot.log 2>/dev/null | tail -1 | sed 's/^== \(.*\) phxboot$/\1/')
    LASTS=$( [ -n "$LAST" ] && date -j -f '%Y-%m-%d %H:%M:%S' "$LAST" +%s 2>/dev/null || echo 0)
    THIS=$( [ "${LASTS:-0}" -ge "${BOOT:-0}" ] && sed -n "/^== $LAST phxboot$/,\$p" /var/log/phxboot.log)
    say "the GPU is not up; phxboot this boot: $(echo "$THIS" | sed -n '2,$p' | tr '\n' '|' | cut -c1-300)"
    if [ -n "$THIS" ] && ! echo "$THIS" | grep -q 'kill switch present'; then
        say "STOP: phxboot already tried a bring-up this boot and it did not finish; a second one is unsafe. Reboot, run this again."
        exit 1
    fi
    [ -x ~/gpu/full.sh ] || [ -f ~/gpu/full.sh ] || { say "STOP: ~/gpu/full.sh missing: bash install.sh first"; exit 1; }
    say "== bringing the GPU up (as phxboot does; full.sh arms a 4-minute reboot in case the GPU hangs the bus)"
    MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash ~/gpu/full.sh kiqsched 2>&1 | grep -E "^compute:|^gfx:|adopt|failed|STOP" | tee -a "$LOG"
    sudo "$MB" claim 2>&1 | tee -a "$LOG"
    for i in $(seq 1 20); do ioreg -rc NMXAccel -d0 | grep -q NMXAccel && break; sleep 1; done
    ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the bring-up did not finish (see bringup.log in the logs)"; exit 1; }
    say "GPU up, NMXAccel running"
fi
say "== GPU state before"
sudo "$MB" gpustat 2>&1 | tee -a "$LOG"
say "== mesboot sdmatest (S1 queue, S2 VRAM copy, S3 system RAM via VMID 7, S4 frames timed, S5 flip): watch the top of the panel at the end"
sudo "$MB" sdmatest 10 2>&1 | tee -a "$LOG"
RC=${PIPESTATUS[0]}
say "== GPU state after"
sudo "$MB" gpustat 2>&1 | tee -a "$LOG"
case $RC in
    0) say "== RESULT: SDMA copies frames and the display flips in the carve-out. Did the panel show five colour bands for 3 s?" ;;
    1) say "== RESULT: not run (see STOP above)" ;;
    *) say "== RESULT: failed at step S$RC (the engine was stopped and reset; WindowServer and Metal are not affected)" ;;
esac
[ -e $G/phxboot-off ] && say "note: phxboot is switched off ($G/phxboot-off); 'sudo bash install_boot.sh' (from the package) turns it on again"
