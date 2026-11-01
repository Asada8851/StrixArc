#!/bin/bash
# radv_check.sh: before the RADV in gpu/vcn (Mesa + mesa-patches-phoenix 0001..0011) replaces the installed one, the
# Metal tests run on both, in their own processes: the Metal plugin opens the RADV NVMTL_ICD_LIB names instead of
# /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib, for that process only. Nothing installed changes, WindowServer
# keeps the installed RADV, no reboot.
#   mtlprobe (compute), mtlprobe render, mtlprobe stress 15 s (compute, blits, sampling, IOSurface textures, many
#   command buffers), yuvprobe quick (YUV surfaces through the plugin) - with the installed RADV, then with the new one
# Run: bash gpu/radv_check.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-radv)
set -u
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=~/nullarc_radv_check.txt; : > "$LOG"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
INSTALLED=${RADV_CHECK_INSTALLED:-/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib}   # (the bench points it elsewhere)
MP=~/gpu/metal/mtlprobe; YP=$HERE/../metal/yuvprobe
T=$(mktemp -d /tmp/radvchk.XXXXXX)
cp "$HERE/vcn/libvulkan_radeon.dylib" "$T/" 2>/dev/null && xattr -d com.apple.quarantine "$T/libvulkan_radeon.dylib" 2>/dev/null
codesign --force --sign - "$T/libvulkan_radeon.dylib" >/dev/null 2>&1
[ -f "$YP" ] && cp "$YP" "$T/yuvprobe" && chmod +x "$T/yuvprobe" && xattr -d com.apple.quarantine "$T/yuvprobe" 2>/dev/null
NEW=$T/libvulkan_radeon.dylib
say() { echo "$*" | tee -a "$LOG"; }
finish() { bash "$LOGS" radv "$LOG" "$T"/*.log | tail -1 | tee -a "$LOG"; rm -rf "$T"; }
trap finish EXIT
lim() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }
say "== radv_check $(date '+%F %T'), $(sw_vers -productVersion)"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot (phxboot)"; exit 1; }
[ -x "$MP" ] && [ -f "$NEW" ] && [ -f "$INSTALLED" ] || { say "STOP: no mtlprobe, gpu/vcn/libvulkan_radeon.dylib or $INSTALLED"; exit 1; }
say "installed RADV: $(shasum "$INSTALLED" | cut -c1-12) $(wc -c < "$INSTALLED" | tr -d ' ') bytes"
say "new RADV:       $(shasum "$NEW" | cut -c1-12) $(wc -c < "$NEW" | tr -d ' ') bytes"

# one test with one RADV: its output and the plugin's log in $T/<tag>-<which>.log, the summary lines in the round's log
run() {   # run <which: installed|new> <tag> <limit s> <command...>
    local which=$1 tag=$2 s=$3; shift 3
    local out="$T/$tag-$which.log" icd=""
    [ "$which" = new ] && icd="NVMTL_ICD_LIB=$NEW"
    lim "$s" /usr/bin/env $icd NVMTL_LOG=1 NVMTL_LOG_PATH="$T/plugin-$tag-$which.log" "$@" > "$out" 2>&1
    local rc=$?
    # which RADV the plugin really opened (its own log line), then what the test says
    local opened=$(grep -m1 -o 'driver opened directly: [^ ]*' "$T/plugin-$tag-$which.log" 2>/dev/null | sed 's/.*: //')
    say "   $tag / $which: rc $rc, RADV ${opened:-? (no plugin log)}"
    grep -E 'results:|pixels:|status|stress\[|stress:|FAIL|PASS|error|rror|synthetic|yuv|planes|OK|ok ' "$out" | grep -v '^ *$' | tail -6 | sed 's/^/      /' | tee -a "$LOG"
    [ $rc = 142 ] && say "   !! $tag / $which hit its time limit"
    echo $rc > "$T/$tag-$which.rc"
}
for which in installed new; do
    say "== $which RADV"
    run $which compute 60 "$MP"
    run $which render 60 "$MP" render
    run $which stress 60 "$MP" stress 15
    [ -x "$T/yuvprobe" ] && run $which yuv 90 "$T/yuvprobe" quick
done
say "== same result with both?"
same=1
for tag in compute render stress yuv; do
    [ -f "$T/$tag-installed.rc" ] || continue
    a=$(cat "$T/$tag-installed.rc"); b=$(cat "$T/$tag-new.rc")
    if [ "$a" = "$b" ]; then say "   $tag: rc $a both"; else say "   $tag: installed rc $a, new rc $b  <-- DIFFERENT"; same=0; fi
done
[ $same = 1 ] && say "== the new RADV passes what the installed one passes" || say "== DIFFERENCES above: the new RADV is not installed until they are understood"
pgrep -x WindowServer >/dev/null && say "WindowServer still runs" || say "WindowServer GONE"
say "== done $(date '+%T')"
