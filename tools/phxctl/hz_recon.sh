#!/bin/bash
# 120 Hz, step 1: what the 880M's display looks like at 3072x1920@60 under macOS, what the panel says over AUX, and
# whether DMCUB takes a command from us. Reads only, but for one command that changes nothing: VBIOS LVTMA_CONTROL
# action 3 (backlight on - it is on already), the command Linux sends last when it switches the panel to 120 Hz.
#   - every display register Linux touches in a 60 <-> 120 Hz switch (dcn_probe.sh with the extended list): the
#     values the firmware chose for 3K@60 are the base the 120 Hz ones are derived from (Linux's own are for its tiled
#     surface, not for our linear one);
#   - DPCD: receiver caps, the eDP link-rate table, the link as the firmware trained it, status, eDP caps;
#   - DMCUB: the INBOX1 ring the firmware left, one command through it (PhxBringup 41: the kext reaches the ring at the
#     top of the 4 GiB carve-out, past the 256 MiB BAR).
# Run: bash gpu/hz_recon.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-hz)
LOG=~/nullarc_hz.txt
HERE=$(cd "$(dirname "$0")" && pwd)
MB=~/gpu/phxctl/mesboot; [ -x "$HERE/phxctl/mesboot" ] && MB="$HERE/phxctl/mesboot"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
: > "$LOG"
say() { echo "$1"; echo "$1" >> "$LOG"; }
run() { say "\$ mesboot $*"; sudo "$MB" "$@" 2>&1 | grep -v 'never been brought up' | tee -a "$LOG"; }
trap 'bash "$LOGS" hz "$LOG" ~/nullarc_dcn.txt | tee -a "$LOG"' EXIT
say "== $(date '+%F %T') $(sw_vers -productVersion) hz_recon"
sudo -v || exit 1
B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
[ "${B:-0}" -ge 41 ] || { say "STOP: PhxBringup build ${B:-?}; this needs 41 (the stick's EFI)"; exit 1; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot"; exit 1; }
say "panel: $(system_profiler SPDisplaysDataType 2>/dev/null | sed -n 's/^ *Resolution: //p' | head -1); SDMA: $(sysctl -n debug.phxscanout_state 2>/dev/null)"

say "== 1. display registers at 3K@60 (dcn_probe.sh -> nullarc_dcn.txt)"
bash "$HERE/dcn_probe.sh" > /dev/null 2>&1; say "   $(grep -c '^0x' ~/nullarc_dcn.txt) registers read"
grep -E 'OTG0_PIXEL_RATE_CNTL|DP_DTO0_|DP0_DP_VID_(M|N) |DP0_DP_LINK_CNTL|DP0_DP_CONFIG|HUBPREQ0_(REF_FREQ|NOM_PARAMETERS_[15]|PER_LINE_DELIVERY |DST_DIMENSIONS|PREFETCH_SETTINGS )|DENTIST' ~/nullarc_dcn.txt | sed 's/^/   /' | tee -a "$LOG"

say "== 2. DMCUB"
run dmubstate

say "== 3. DPCD (reads only)"
for a in "0x00000 16" "0x00010 16" "0x0000e 1" "0x00100 16" "0x00115 1" "0x00200 8" "0x00600 1" "0x00700 8" "0x02200 16"; do run dpcd r $a; done

say "== 4. one DMCUB command: VBIOS LVTMA_CONTROL action 3 (backlight on; it is on)"
run dmub "800f0000 03000000"
say "   the panel should not have changed. Did it flicker or go dark? say so."
say "== done $(date '+%T')"
