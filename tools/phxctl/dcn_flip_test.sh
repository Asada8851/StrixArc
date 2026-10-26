#!/bin/bash
# Strix Point: can the display engine scan out of system RAM, set up the way Linux sets a pipe? (`mesboot dcnflip`:
# five colour bands in a contiguous system-RAM buffer, DCN AGP + the pipe's system aperture + its L1 TLB in
# SYSTEM_ACCESS_MODE 3, a flip for 5 s, everything put back.) dcn_agp_test.sh, before it, left the pipe's aperture and
# TLB as the firmware had them (0): the address went out untranslated and the panel read all ones (white).
# Also logs where WindowServer's frames are: copied from the IOSurface's own pages or from PhxAccel's VRAM pool.
# Run (on a WindowServer-on-GPU boot): bash gpu/dcn_flip_test.sh  ->  send ~/nullarc_flip.txt and say what the panel
# showed during the 5 s (colour bands at the top / white / black / no change)
LOG=~/nullarc_flip.txt
HERE=$(cd "$(dirname "$0")" && pwd)
MB=~/gpu/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
say "== $(date '+%F %T') $(sw_vers -productVersion) dcn_flip_test"
ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB || { say "STOP: PhxAMDFB does not have the panel (needs a WindowServer-on-GPU boot)"; exit 1; }
grep -a -q 'dcnflip: STOP' "$MB" || { say "STOP: $MB has no dcnflip: use the mesboot of nullarc-strix-27"; exit 1; }
sudo -v || exit 1
say "== where WindowServer's frames come from (PhxAccel counters)"
for k in nvaccel_iop_ok nvaccel_iop_src_surf nvaccel_iop_src_res nvaccel_iop_copy_unwritten nvaccel_iop_copy_skip \
         nvaccel_vm_alloc nvaccel_vm_alloc_bytes nvaccel_vm_dealloc_bytes nvaccel_iop_flip_novram nvaccel_iop_flip_unwritten; do
    say "   $k = $(sysctl -n debug.$k 2>/dev/null)"
done
say "== the experiment: LOOK AT THE TOP OF THE PANEL (writing the bands takes a few seconds first)"
sudo "$MB" dcnflip 5 2>&1 | while IFS= read -r l; do say "   $l"; done
say "   what did the panel show during the 5 s? (colour bands at the top / white / black / no change)"
for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
