#!/bin/bash
# Strix Point, read only: the display engine's state under macOS, for zero-copy scan-out (DCN fetches WindowServer's
# finished frame itself instead of PhxAccel copying it into the firmware's framebuffer by CPU).
#   - pipe 0: HUBP/HUBPREQ/HUBPRET (surface address, format, pitch, flip control, viewport), cursor, DPP/DSCL, MPC/OPP,
#     OTG0: what the firmware set up and macOS scans today;
#   - DCHUBBUB and DCN_VM: the FB / AGP / system apertures and the VM contexts: which addresses the display can fetch
#     from (the carve-out only, or system RAM too - where PhxAccel's surfaces live);
#   - DMCUB: whether the display microcontroller runs (needed later for link training);
#   - display clocks; MMHUB's FB / AGP / system apertures.
# Only plain registers (the list skips data ports, FIFOs, LUTs) of blocks Linux itself accesses on this machine; nothing
# is written. One control read first: a powered-down display engine stops the probe instead of being read 767 times.
# Needs: the GPU up (phxboot), dcn_regs.txt next to this script. Run: bash dcn_probe.sh  ->  send ~/nullarc_dcn.txt
LOG=~/nullarc_dcn.txt
HERE=$(cd "$(dirname "$0")" && pwd)
LIST="$HERE/dcn_regs.txt"; [ -f "$LIST" ] || LIST=~/gpu/dcn_regs.txt
PX=~/gpu/phxctl/phxctl; [ -x "$HERE/phxctl/phxctl" ] && PX="$HERE/phxctl/phxctl"
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
[ -f "$LIST" ] || { echo "STOP: no dcn_regs.txt"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
say "== $(date '+%F %T') $(sw_vers -productVersion) dcn_probe"
[ "$(ioreg -l | sed -n 's/.*"phx-chip" = "\(.*\)".*/\1/p' | head -1)" = strix-point ] || { say "STOP: PhxBringup/Strix not up"; exit 1; }
sudo -v || exit 1
say "   PhxBringup build $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1); PhxAMDFB on the panel: $(ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && echo yes || echo no)"
ioreg -rc PhxBringup -d1 | grep -E '"phx-fb-(mc-base|size|phys)"' | sed 's/^ */   /' | while IFS= read -r l; do say "$l"; done
ioreg -rc PhxAMDFB -d1 | grep -E '"PhxFB(Width|Height|RefreshMilliHz|HTotal|VTotal)"' | sed 's/^ */   /' | while IFS= read -r l; do say "$l"; done
say "   frames: scan-out view $(sysctl -n debug.phxfb_alias_view 2>/dev/null), copied $(sysctl -n debug.nvaccel_iop_ok 2>/dev/null)"

# control read: OTG0_OTG_CONTROL (the panel's timing generator; PhxAMDFB and try15 read it before): must have its
# enable bit and not look like a dead bus
CTL=$(sudo "$PX" regs 0x5003 | awk '{print $NF}')
say "   control read OTG0_OTG_CONTROL = $CTL"
case "$CTL" in 0x*) ;; *) say "STOP: no answer from phxctl"; exit 1 ;; esac
if [ "$CTL" = 0xffffffff ] || [ $(( CTL & 1 )) = 0 ]; then say "STOP: OTG0 does not look powered and running"; exit 1; fi

say "== registers (address name value)"
# phxctl regs prints "0xADDR: 0xVALUE" per register; 64 per call
awk '{print $1}' "$LIST" | xargs -n 64 sudo "$PX" regs > ~/nullarc_dcn.raw 2>&1
awk 'NR == FNR { n[tolower($1)] = $2; next }
     { a = tolower($1); sub(/:$/, "", a); v = $NF; err = ($0 ~ /error/) ? " (error)" : ""
       printf "%s %-56s %s%s\n", a, n[a], v, err }' "$LIST" ~/nullarc_dcn.raw >> "$LOG"
say "   $(grep -c '^0x' "$LOG") registers read, $(grep -c '(error)' "$LOG") errors"
rm -f ~/nullarc_dcn.raw
for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && cp "$LOG" "$v/" && say "(log copied to $v)"; done
say "== done $(date '+%T'). Send $LOG"
