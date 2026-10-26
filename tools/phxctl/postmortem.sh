#!/bin/bash
# After a hang + watchdog reboot: what the plugin, RADV and the kexts said before it. The system log survives reboots.
# usage: bash ~/gpu/postmortem.sh "2026-10-25 14:25:00" "2026-10-25 14:35:00"
S=${1:?start time "YYYY-MM-DD HH:MM:SS"}; E=${2:-$(date '+%F %T')}; OUT=~/nullarc_postmortem.txt
log show --start "$S" --end "$E" --style compact --info --predicate \
  'eventMessage CONTAINS "NVMTL" OR eventMessage CONTAINS "NMXAccel" OR eventMessage CONTAINS "PhxBringup" OR eventMessage CONTAINS "radv" OR process == "mtlprobe" OR sender == "PhxBringup" OR sender == "PhxAccel"' \
  2>/dev/null | grep -v 'cfg+0x' > "$OUT"
wc -l < "$OUT"; tail -120 "$OUT"; echo "== full: $OUT"
