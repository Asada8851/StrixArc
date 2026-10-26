#!/bin/bash
# nullarc_logs.sh <tag> [file...]: everything needed to tell what happened, saved to the USB stick (the volume that holds
# the nullarc-strix-*.zip packages), else to ~/nullarc_logs. Every test script calls it on exit, the given files (its own
# log) included; after a hang, a black screen or an odd boot, run it by hand: bash ~/gpu/nullarc_logs.sh afterhang
#   boot.txt      this boot: the kexts, the plugin, RADV, phxboot, WindowServer errors (unified log, filtered)
#   prevboot.txt  the 3 hours before this boot, same filter: what the boot that hung or rebooted said
#   state.txt     versions, boot-args, NVRAM, phxboot switches, ioreg of PhxBringup/PhxAMDFB/NMXAccel, debug.* sysctls
#   phxboot.log, bringup logs, the snapshots phxboot takes at boot ($G/logs), crash/panic reports of the last 6 hours
# Never asks for a password: what needs root is taken only when sudo works without one (or when run as root).
# NULLARC_LOGS_DEST=<dir> writes there instead (phxboot: $G/logs at every boot, picked up by the next run with the stick).
# NULLARC_LOGS_SINCE / NULLARC_LOGS_UNTIL="YYYY-MM-DD HH:MM:SS" add window.txt: the same filter over that span (an
# older boot; the unified log keeps days).
TAG=${1:-manual}; [ $# -gt 0 ] && shift
G=/Users/Shared/gpu
DEST=${NULLARC_LOGS_DEST:-}
[ -n "$DEST" ] || for v in /Volumes/*; do ls "$v"/nullarc-strix-*.zip >/dev/null 2>&1 && { DEST="$v/nullarc_logs"; break; }; done
[ -n "$DEST" ] || DEST=~/nullarc_logs
D="$DEST/$(date '+%Y%m%d-%H%M%S')-$TAG"
mkdir -p "$D" 2>/dev/null || { D=~/nullarc_logs/$(date '+%Y%m%d-%H%M%S')-$TAG; mkdir -p "$D"; }
S=""; [ "$(id -u)" = 0 ] || { sudo -n true 2>/dev/null && S="sudo -n"; }
# "{ sec = 1761501234, usec = 0 } ...": the first number (a greedy .*sec matched usec: boot read as 1970, 2026-10-26)
BOOT=$(sysctl -n kern.boottime | sed -n 's/^{ sec = \([0-9]*\),.*/\1/p')
PRED='sender == "PhxBringup" OR sender == "PhxAccel" OR eventMessage CONTAINS "PhxBringup" OR eventMessage CONTAINS "NMXAccel"
  OR eventMessage CONTAINS "PhxFB" OR eventMessage CONTAINS "PhxAMDFB" OR eventMessage CONTAINS "NVMTL"
  OR eventMessage CONTAINS "radv" OR eventMessage CONTAINS "phxboot" OR eventMessage CONTAINS "GPU Restart"
  OR eventMessage CONTAINS[c] "panic" OR eventMessage CONTAINS "IOAccel" OR process == "mesboot" OR process == "mtlprobe"
  OR (process == "WindowServer" AND (messageType == error OR messageType == fault))'
# WindowServer on the firmware framebuffer with Metal around logs "Unsupported sw composite destination" ~200 times a
# second: counted in state.txt, kept out of the logs. A log longer than 30000 lines keeps its first and last 15000.
SPAM='Unsupported sw composite destination'
trim() { local t="$1.tmp"; grep -v -e 'cfg+0x' -e "$SPAM" > "$t"; if [ "$(wc -l < "$t")" -gt 30000 ]; then
         { head -15000 "$t"; echo "... ($(($(wc -l < "$t") - 30000)) lines left out) ..."; tail -15000 "$t"; } > "$1"; else mv "$t" "$1"; fi; rm -f "$t"; }
# each log query gets 60 s at most (the log process itself is killed then): a full read of the unified log took minutes
# and looked like a hang after a test's "done" (2026-10-27)
limit() { "$@" & local p=$!; ( sleep 60; kill $p ) >/dev/null 2>&1 & local w=$!; wait $p 2>/dev/null; kill $w 2>/dev/null; }   # the watcher keeps no output open: "| tee" would wait for it
q() { limit log show "$@" --style compact --info --predicate "$PRED"; }
echo "== collecting logs for the USB stick (up to a minute)..."
# the log queries are the slow part: run them side by side
( q --last 2h > "$D/boot.txt.raw"; trim "$D/boot.txt" < "$D/boot.txt.raw"; rm -f "$D/boot.txt.raw" ) &
P1=$!
if [ -n "$BOOT" ]; then
    ( q --start "$(date -r $((BOOT - 10800)) '+%Y-%m-%d %H:%M:%S')" --end "$(date -r "$BOOT" '+%Y-%m-%d %H:%M:%S')" > "$D/prevboot.txt.raw"
      trim "$D/prevboot.txt" < "$D/prevboot.txt.raw"; rm -f "$D/prevboot.txt.raw" ) &
    P2=$!
fi
if [ -n "${NULLARC_LOGS_SINCE:-}" ]; then
    ( q --start "$NULLARC_LOGS_SINCE" --end "${NULLARC_LOGS_UNTIL:-$(date '+%Y-%m-%d %H:%M:%S')}" > "$D/window.txt.raw"
      trim "$D/window.txt" < "$D/window.txt.raw"; rm -f "$D/window.txt.raw" ) &
    P3=$!
fi
{
    echo "== $(date '+%F %T') $TAG"; sw_vers 2>/dev/null; uname -a
    echo "boot: $(date -r "${BOOT:-0}" '+%F %T') (uptime: $(uptime))"
    echo "boot-args: $(nvram boot-args 2>/dev/null | cut -f2-)"; echo "phxfb-boot: $(nvram phxfb-boot 2>/dev/null | cut -f2-)"
    echo "== $G"; ls -la "$G" 2>&1
    echo "== kexts"; kextstat 2>/dev/null | grep -iE 'phx|nmx|lilu|whatevergreen' ; kmutil showloaded 2>/dev/null | grep -iE 'phx|nmx'
    echo "== PhxBringup"; ioreg -rc PhxBringup -d1 2>&1 | grep -E '"phx-|class' | head -40
    echo "== display"; ioreg -l 2>/dev/null | grep -E '"(DisplayVendorID|DisplayProductID|DisplaySerialNumber|IOFBCurrentPixelCount|IOFBCurrentPixelClock)"' | sort -u | head
    system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Resolution|Refresh|UI Looks|Main Display|Display Type' | head
    echo "== framebuffer"; ioreg -rc IOFramebuffer -d0 2>&1 | grep -E 'class' ; ioreg -rc PhxAMDFB -d1 2>&1 | grep -vE 'IOFB|IOPower|Interrupt' | head -30
    echo "== NMXAccel"; ioreg -rc NMXAccel -d1 2>&1 | grep -E 'class|PerformanceStatistics' | head -5
    echo "== processes"; pgrep -lx WindowServer; pgrep -fl 'mesboot|mtlprobe|fpsanim|phxboot' 2>/dev/null
    echo "== sysctl"; sysctl debug 2>/dev/null | grep -E 'phx|nvaccel'
    echo "== Metal allow list"; cat /Library/GPUBundles/nvmtl-allow.txt 2>&1 | head -12
} > "$D/state.txt" 2>&1
tail -400 /var/log/phxboot.log > "$D/phxboot.log" 2>/dev/null
# PhxBringup's IOLog lines do not reach the unified log on macOS 15 (none in boot.txt, 2026-10-26): the kernel's buffer
[ -n "$S" ] || [ "$(id -u)" = 0 ] && $S dmesg 2>/dev/null | grep -E 'PhxBringup|PhxFB|NMXAccel|PhxAccel' | tail -600 > "$D/dmesg.txt"
for f in ~/gpu/bringup.log "$G/bringup.log" ~/gpu/fwaddr.log; do [ -f "$f" ] && cp "$f" "$D/$(basename "$(dirname "$f")")-$(basename "$f")"; done
# phxboot's snapshot of the boot before (taken at boot, when the stick may not be mounted yet)
[ -z "${NULLARC_LOGS_DEST:-}" ] && [ -d "$G/logs" ] && find "$G/logs" -mindepth 1 -maxdepth 1 -mmin -720 -exec cp -R {} "$D/" \; 2>/dev/null
mkdir -p "$D/reports"
for dir in /Library/Logs/DiagnosticReports ~/Library/Logs/DiagnosticReports; do
    find "$dir" -maxdepth 1 -type f -mmin -360 \( -name '*.panic' -o -name 'Kernel*' -o -name 'WindowServer*' -o -name '*GPU*' \
        -o -name 'mtlprobe*' -o -name 'fpsanim*' -o -name 'mesboot*' -o -name '*.ips' \) 2>/dev/null |
        while IFS= read -r f; do $S cp "$f" "$D/reports/" 2>/dev/null; done
done
ls -la /Library/Logs/DiagnosticReports 2>/dev/null | tail -30 > "$D/reports/list.txt"
for f in "$@"; do [ -f "$f" ] && cp "$f" "$D/"; done
wait $P1 ${P2:-} ${P3:-} 2>/dev/null
$S chown -R "$(id -u)" "$D" 2>/dev/null
sync
echo "logs: $D"
