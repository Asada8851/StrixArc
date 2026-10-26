#!/bin/bash
# Strix Point: can the display engine scan a frame out of system RAM? (The step before zero-copy scan-out.)
# dcn_probe showed DCN fetching only the carve-out (FB 0x80_0000_0000.., the firmware's frame at its start) with its AGP
# aperture off (DCN_VM_AGP_BOT/TOP/BASE = 0). PhxAccel's surfaces - WindowServer's frames among them - live in a
# physically contiguous pool in system RAM ("PhxFB: VRAM pool: ... at phys ..."). This turns the DCN AGP aperture on so
# that MC addresses 0x10_0000_0000 + P mean system physical address P, points pipe 0 at the pool for 4 s, half a screen
# in (so the panel shows something unmistakable: WindowServer's frame moved up by half), reads back which address the
# display fetched (SURFACE_EARLIEST_INUSE) and its underflow status, and puts every register back.
#   stage A: the DCN aperture only; stage B: the same in MMHUB too (DCN fetches go through MMHUB translation).
# The flip is the one Linux does (address high, then low; latched at the next vsync). If the panel goes black or shows
# noise, wait: everything is restored after 4 s by itself. A hang (very unlikely: the pool is ordinary RAM) needs a
# hold of the power button; nothing is stored, the next boot is as before.
# Run: bash gpu/dcn_agp_test.sh  ->  send ~/nullarc_agp.txt
LOG=~/nullarc_agp.txt
HERE=$(cd "$(dirname "$0")" && pwd)
PX=~/gpu/phxctl/phxctl; [ -x "$HERE/phxctl/phxctl" ] && PX="$HERE/phxctl/phxctl"
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
rd() { sudo "$PX" regs "$@" | awk '{print $NF}'; }
wr() { sudo "$PX" wreg "$1" "$2" > /dev/null; }
say "== $(date '+%F %T') $(sw_vers -productVersion) dcn_agp_test"
ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB || { say "STOP: PhxAMDFB does not have the panel (needs a WindowServer-on-GPU boot)"; exit 1; }
sudo -v || exit 1
# registers (dcn_3_5_0 / mmhub_3_3_0 with the 880M's bases)
AGP_BOT=0x3938 AGP_TOP=0x3939 AGP_BASE=0x393a
MM_TOP=0x1a9ee MM_BOT=0x1a9ef MM_BASE=0x1a9f0
ADDR_HI=0x3acb ADDR_LO=0x3aca INUSE_LO=0x3ae4 INUSE_HI=0x3ae5 FLIPC=0x3adb HUBP_CNTL=0x3ab3 PITCH=0x3ac7 VIEWPORT=0x3aaa
POOL=$(log show --last boot --style compact --predicate 'eventMessage CONTAINS "VRAM pool"' 2>/dev/null |
       sed -n 's/.*VRAM pool: [0-9]* MB at phys \(0x[0-9a-f]*\).*/\1/p' | tail -1)
[ -n "$POOL" ] || { say "STOP: no 'PhxFB: VRAM pool' line in this boot's log"; exit 1; }
set -- $(rd $AGP_BOT $AGP_TOP $AGP_BASE $MM_BOT $MM_TOP $MM_BASE $ADDR_HI $ADDR_LO $HUBP_CNTL $PITCH $VIEWPORT)
O_BOT=$1 O_TOP=$2 O_BASE=$3 O_MBOT=$4 O_MTOP=$5 O_MBASE=$6 O_HI=$7 O_LO=$8 O_CNTL=$9 O_PITCH=${10} O_VP=${11}
say "   pool at phys $POOL; now: DCN AGP bot/top/base $O_BOT/$O_TOP/$O_BASE, MMHUB AGP $O_MBOT/$O_MTOP/$O_MBASE"
say "   pipe 0: surface $O_HI:$O_LO pitch $O_PITCH viewport $O_VP DCHUBP_CNTL $O_CNTL"
[ "$O_HI" = 0x00000080 ] && [ "$O_LO" = 0x00000000 ] || { say "STOP: pipe 0 does not scan the firmware's frame at 0x80:00000000"; exit 1; }
[ "$O_BOT$O_TOP$O_BASE" = 0x000000000x000000000x00000000 ] || { say "STOP: the DCN AGP aperture is already set: not touching it"; exit 1; }
restore() {
    wr $ADDR_HI $O_HI; wr $ADDR_LO $O_LO; sleep 0.1
    wr $AGP_BASE $O_BASE; wr $AGP_TOP $O_TOP; wr $AGP_BOT $O_BOT
    wr $MM_BASE $O_MBASE; wr $MM_TOP $O_MTOP; wr $MM_BOT $O_MBOT
}
trap 'restore; say "   (restored)"' EXIT INT TERM
# the aperture: MC 0x10_0000_0000 .. 0x18_C0FF_FFFF -> system physical 0 .. 0x8_C0FF_FFFF (all of this RAM), in 16 MB units
BOT=0x1000 TOP=0x18c0 BASE=0x0
PITCHB=$(( ((O_PITCH & 0x3fff) + 1) * 4 )); ROWS=$(( ((O_VP >> 16) & 0x3fff) ))
TARGET=$(( 0x1000000000 + POOL + (ROWS / 2) * PITCHB ))
T_HI=$(printf '0x%x' $(( TARGET >> 32 ))); T_LO=$(printf '0x%08x' $(( TARGET & 0xffffffff )))
say "   target: MC $T_HI:$T_LO = pool + $(( ROWS / 2 )) rows of $PITCHB bytes"
stage() {   # stage <label> : flip to the pool and read back what the display fetches
    local label=$1
    wr $HUBP_CNTL $(( O_CNTL | 0x80000000 ))   # clear any old underflow status
    wr $ADDR_HI $T_HI; wr $ADDR_LO $T_LO
    sleep 0.2
    set -- $(rd $INUSE_HI $INUSE_LO $FLIPC $HUBP_CNTL)
    say "   $label: in use $1:$2 -> the display $( [ "$(( $1 & 0xffff ))" = "$(( T_HI ))" ] && [ "$2" = "$T_LO" ] && echo "asks for the pool address (what it gets back: see the panel)" || echo "did NOT take the new address"); flip control $3; underflow $(( ($4 >> 28) & 7 ))"
}
# 2026-10-26, first run: stage A (DCN only) made the panel WHITE for its 4 s while SURFACE_EARLIEST_INUSE showed the
# pool address and no underflow: the display asked for the right address and read all ones (0xffffffff = white), the
# answer of a physical address no RAM sits at. DCN's requests go through MMHUB's translation, whose AGP aperture was
# off. So both stages always run now, each announced first; say what the panel showed in each.
say "== stage A: the DCN AGP aperture only (the first run: a white panel). Starts in 3 s, lasts 4 s"
sleep 3
wr $AGP_BASE $BASE; wr $AGP_TOP $TOP; wr $AGP_BOT $BOT
stage A
sleep 4
wr $ADDR_HI $O_HI; wr $ADDR_LO $O_LO; sleep 0.2
set -- $(rd $INUSE_HI $INUSE_LO $HUBP_CNTL); say "   back: in use $1:$2, underflow $(( ($3 >> 28) & 7 ))"
say "== stage B: the DCN and MMHUB AGP apertures, as Linux sets them (the desktop moved up by half = it works). Starts in 3 s, lasts 4 s"
sleep 3
wr $MM_BASE $BASE; wr $MM_TOP $TOP; wr $MM_BOT $BOT
stage B
sleep 4
restore; trap - EXIT INT TERM
set -- $(rd $ADDR_HI $ADDR_LO $INUSE_HI $INUSE_LO $AGP_BOT $MM_BOT $HUBP_CNTL)
say "   restored: surface $1:$2, in use $3:$4, AGP bot DCN $5 MMHUB $6, underflow $(( ($7 >> 28) & 7 ))"
say "   what did the panel show in stage A and in stage B? (moved desktop / white / black / noise / no change)"
for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
