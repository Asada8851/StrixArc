#!/bin/bash
# vcn_decode.sh: H.264 and HEVC decoded by the 880M's video engine (VCN 4.0.5) through RADV's Vulkan Video, in FFmpeg
# and in its player ffplay (phase 2.4). Nothing installed changes: this round's RADV, FFmpeg/ffplay and vkshim run from
# a copy in /tmp; WindowServer keeps the installed RADV. VCN is up only while this runs.
#   1. mesboot vcn up / ringtest / adopt, as in vcn_test.sh: the ring goes to PhxBringup (build 43+)
#   2. FFmpeg 8.1 (ours, with Vulkan) on each clip in the stick's video/: a short decode on VCN, then every frame's
#      checksum from VCN against the CPU decoder's here and against Linux's (video/ref, made on this 880M under Linux):
#      they must be identical, frame for frame
#   3. CPU time of the decode alone (-f null): VCN against the CPU decoder
#   4. ffplay (ours: ffmpeg-patches/0001 copies VCN's frames to memory for SDL's Metal renderer) plays the 4K clips on
#      VCN, one on the CPU for comparison, and 30 s of the clip with sound; logged: the texture format (NV12 = the
#      frames came from VCN), dropped frames (fd=), the CPU time of the whole playback
#   5. vcn release / down
# A reboot is armed for 10 minutes and cancelled at the end: if the display or the machine stops, it comes back by itself.
# Run: bash gpu/vcn_decode.sh (go.sh does)           -> logs on the USB stick (nullarc_logs/<time>-vcndec)
#      bash gpu/vcn_decode.sh play <file> [ffplay opts] -> one file of yours in ffplay on VCN (logs: <time>-vcnplay)
#      bash gpu/vcn_decode.sh perf                     -> where playback spends its CPU (logs: <time>-vcnperf): the decode
#        alone, decode + copy to memory, the CPU decoder (ffmpeg -benchmark); ffplay on VCN (staging copy / per-frame
#        import) / on the CPU / 10-bit with
#        4 s of `sample` profile each; per step the MMHUB flushes (buffer mappings) and WindowServer's CPU time
#      bash gpu/vcn_decode.sh shared                   -> ffplay with the Metal plugin and FFmpeg on one and the same
#        RADV file (the plugin told by NVMTL_ICD_LIB), as once that RADV is installed (logs: <time>-vcnshared)
set -u
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=test; PLAY=""; PLAYLIM=120
if [ "${1:-}" = play ]; then
    MODE=play; PLAY=${2:?usage: vcn_decode.sh play <file> [ffplay options]}; shift 2; PLAYLIM=540   # under the 10 min reboot
    [ -f "$PLAY" ] || { echo "no file $PLAY"; exit 1; }
elif [ "${1:-}" = perf ]; then
    MODE=perf
elif [ "${1:-}" = shared ]; then
    MODE=shared
fi
LOG=~/nullarc_vcn_decode.txt; : > "$LOG"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
VID=$HERE/../video
# this round's mesboot, firmware and RADV sit in gpu/vcn/, FFmpeg and ffplay in gpu/ffmpeg/{bin,lib}: copied off the
# stick first (exec bits, no quarantine, ad hoc signatures as the copies need)
T=$(mktemp -d /tmp/vcndec.XXXXXX)
cp "$HERE/vcn/mesboot" "$HERE/vcn/vcn_4_0_5.bin" "$HERE/vcn/libvulkan_radeon.dylib" "$T/" 2>/dev/null
mkdir -p "$T/ff" && cp -R "$HERE/ffmpeg/bin" "$HERE/ffmpeg/lib" "$T/ff/" 2>/dev/null
chmod +x "$T/mesboot" "$T"/ff/bin/* 2>/dev/null
xattr -dr com.apple.quarantine "$T" 2>/dev/null
for f in "$T/libvulkan_radeon.dylib" "$T"/ff/lib/*.dylib "$T"/ff/bin/*; do codesign --force --sign - "$f" >/dev/null 2>&1; done
MB=$T/mesboot; FW=$T/vcn_4_0_5.bin
# ffplay on VCN: frames copied through FFmpeg's pooled staging buffers, not by importing each frame's memory (a kext
# host import, map and unmap per frame: round 57 measured the staging copy cheaper, 0.7 mappings a frame against 4.6)
VCNPLAY="-hwaccel vulkan -hwaccel_opts avoid_host_import=1"
# what every FFmpeg/ffplay run gets: our libraries first, vkshim (lib/libvulkan.dylib) hands them this round's RADV. Set
# with env right before the program: SIP-protected programs in between (perl, time) drop DYLD_* from what they pass on.
VENV="DYLD_LIBRARY_PATH=$T/ff/lib STRIXARC_RADV=$T/libvulkan_radeon.dylib VKSHIM_VERBOSE=1"
say() { echo "$*" | tee -a "$LOG"; }
ADOPTED=""; UP=""
finish() {
    # VCN goes down only once PhxBringup has let go of the ring: down under a ring the kext still feeds would leave its
    # jobs unanswered
    if [ -n "$ADOPTED" ]; then M vcn release || { sleep 3; M vcn release; } && ADOPTED=""; fi
    if [ -n "$UP" ] && [ -z "$ADOPTED" ]; then M vcn down; fi
    [ -n "$ADOPTED" ] && say "!! PhxBringup still holds the VCN ring (jobs running?): VCN left up until the reboot"
    say "MMHUB flushes without the semaphore: $(sysctl -n debug.phxvcn_sem_miss 2>/dev/null), not acked: $(sysctl -n debug.phxvcn_ack_miss 2>/dev/null)"
    say "== PhxBringup's kernel lines:"
    sudo -n dmesg 2>/dev/null | grep -E 'PhxBringup: (adopted|VCN|MMHUB|released)' | tail -12 | tee -a "$LOG"
    pgrep -x WindowServer >/dev/null && say "WindowServer still runs (pid $(pgrep -x WindowServer))" || say "WindowServer GONE"
    sudo -n killall shutdown 2>/dev/null && say "== the armed reboot is cancelled"
    say "== done $(date '+%T')"
    sudo -n dmesg 2>/dev/null | tail -60 > ~/nullarc_vcn_decode_dmesg.txt
    local tag=vcndec; [ $MODE = play ] && tag=vcnplay; [ $MODE = perf ] && tag=vcnperf; [ $MODE = shared ] && tag=vcnshared
    bash "$LOGS" $tag "$LOG" ~/nullarc_vcn_decode_dmesg.txt "$T"/*.log | tail -1 | tee -a "$LOG"
    rm -rf "$T"
}
M() { say "\$ mesboot $*"; sudo "$MB" "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }
# a program with a time limit (perl alarm, as vcn_test.sh does): a stuck decode must not hold the round
lim() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }
FF() { lim "$1" /usr/bin/env $VENV "$T/ff/bin/ffmpeg" -hide_banner -nostdin -nostats "${@:2}"; }
# a run killed by its time limit (rc 142, SIGALRM) means VCN or the kext stopped answering: the rest would only wait out
# its own limits past the armed reboot, so the round ends there (finish releases what it can and saves the logs)
HUNG=""
hung() { [ "$1" = 142 ] && { HUNG=1; say "!! $2 hit its time limit: VCN does not answer, the round stops here"; }; }
# the frame checksums of a framemd5 file, one per line
sums() { grep -v '^#' "$1" | awk -F, '{gsub(/ /, "", $NF); print $NF}'; }

sudo -v || exit 1
trap finish EXIT
say "== vcn_decode ($MODE) $(date '+%F %T'), $(sw_vers -productVersion), PhxBringup $(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { say "STOP: the GPU is not up this boot (phxboot)"; exit 1; }
[ -x "$MB" ] && [ -f "$FW" ] && [ -f "$T/libvulkan_radeon.dylib" ] && [ -x "$T/ff/bin/ffmpeg" ] && [ -x "$T/ff/bin/ffplay" ] &&
    [ -f "$T/ff/lib/libvulkan.dylib" ] ||
    { say "STOP: gpu/vcn or gpu/ffmpeg incomplete on the stick"; exit 1; }
say "FFmpeg: $(lim 20 /usr/bin/env $VENV "$T/ff/bin/ffmpeg" -hide_banner -version 2>&1 | head -1)"
sudo bash -c 'nohup shutdown -r +10 >/dev/null 2>&1 &'; say "== reboot armed for 10 minutes (cancelled at the end)"
sync

M vcn status
# VCN already up and held by PhxBringup (phxboot with $G/phxboot-vcn): used as it is, and left so at the end
if ioreg -rc PhxBringup -d1 | grep -q '"phx-vcn" = 1'; then
    say "== VCN is up since boot and held by PhxBringup: used as it is, left up at the end"
else
    UP=1
    M vcn up "$FW" || { say "STOP: vcn up failed"; exit 1; }
    M vcn ringtest 3 || { say "STOP: the ring test failed"; exit 1; }
    M vcn adopt "$FW" || { say "STOP: adopt failed"; exit 1; }
    ADOPTED=1
fi
say "VCN jobs so far: $(sysctl -n debug.phxvcn_jobs 2>/dev/null || echo '? (PhxBringup before 44)')"

# ffplay with a time limit and the CPU time of the whole run; its log (-v verbose) goes to $T/ffplay-<tag>.log, one
# summary line to the round's log: texture format (NV12: frames from VCN), the last status line's dropped frames
PLAY() {
    local tag=$1; shift
    local log="$T/ffplay-$tag.log"
    say "\$ ffplay $*"
    lim $PLAYLIM /usr/bin/time -p /usr/bin/env $VENV "$T/ff/bin/ffplay" -hide_banner -autoexit -loglevel verbose -x 1600 \
        "$@" < /dev/null 2> "$log"
    local rc=$?
    local tex=$(grep -o 'texture with SDL_PIXELFORMAT_[A-Z0-9]*' "$log" | head -1 | sed 's/.*_//')
    local fd=$(tr '\r' '\n' < "$log" | grep 'fd=' | tail -1 | sed -n 's/.*fd= *\([0-9]*\).*/\1/p')
    # time's lines follow ffplay's last status line, which ends in \r, not \n
    local cpu=$(tr '\r' '\n' < "$log" | awk '/^(user|sys) /{s+=$2} END {printf "%.2f", s}')
    local real=$(tr '\r' '\n' < "$log" | awk '/^real /{print $2}')
    grep -E 'vkshim|copied to memory|rror|ailed|reject|LOST' "$log" | head -6 | tee -a "$LOG"
    say "   ffplay $tag: exit $rc, texture ${tex:-?}, dropped ${fd:-?}, CPU ${cpu}s over ${real:-?}s"
    hung $rc "ffplay $tag" && return 1
    return 0
}

if [ $MODE = shared ]; then
    # one RADV image for both Vulkan users in the process: the plugin's (SDL's Metal renderer) and FFmpeg's (vkshim)
    say "== shared: the Metal plugin and FFmpeg in ffplay on the same RADV file ($T/libvulkan_radeon.dylib)"
    cp "$VID/hevc_2160.mp4" "$T/hevc_2160.mp4" || { say "STOP: no $VID/hevc_2160.mp4"; exit 1; }
    VENV="$VENV NVMTL_ICD_LIB=$T/libvulkan_radeon.dylib NVMTL_LOG=1 NVMTL_LOG_PATH=$T/plugin-shared.log"
    PLAY shared-hevc $VCNPLAY "$T/hevc_2160.mp4" || exit 1
    opened=$(grep -m1 -o 'driver opened directly: [^ ]*' "$T/plugin-shared.log" 2>/dev/null | sed 's/.*: //')
    ff=$(grep -m1 -o 'vkshim: Vulkan from .*' "$T/ffplay-shared-hevc.log" | sed 's/vkshim: Vulkan from //')
    say "   the plugin opened ${opened:-? (no plugin log)}, FFmpeg's vkshim ${ff:-?}"
    # dyld reports /tmp as /private/tmp (vkshim's dladdr), the plugin its environment's string
    [ -n "$opened" ] && [ "${opened#/private}" = "${ff#/private}" ] && say "   one RADV file for both: as after the install" || say "   !! not the same file: this run does not show the installed case"
    [ -f "$VID/user_av.mp4" ] && { PLAY shared-user_av $VCNPLAY -t 20 "$VID/user_av.mp4" || exit 1; }
    exit 0
fi

if [ $MODE = perf ]; then
    say "== perf: hevc_2160 (4K HEVC, 300 frames) and hevc10_2160, VCN against the CPU"
    for c in hevc_2160 hevc10_2160; do cp "$VID/$c.mp4" "$T/$c.mp4" || { say "STOP: no $VID/$c.mp4"; exit 1; }; done
    flushes() { sysctl -n debug.phxvcn_sem_miss 2>/dev/null || echo 0; }
    WSPID=$(pgrep -x WindowServer | head -1)
    # a process's CPU time in seconds ([[hh:]mm:]ss.cc from ps)
    cputime() { ps -o time= -p "$1" 2>/dev/null | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%.2f", s }'; }
    STEP_F=0; STEP_W=0
    step_begin() { STEP_F=$(flushes); STEP_W=$(cputime "$WSPID"); }
    step_end() {   # step_end <tag> <frames>
        local f=$(( $(flushes) - STEP_F )) w=$(cputime "$WSPID")
        say "   $1: MMHUB flushes $f ($(awk -v f=$f -v n=$2 'BEGIN { printf "%.1f", f / n }') per frame), WindowServer CPU +$(awk -v a=$STEP_W -v b=${w:-0} 'BEGIN { printf "%.2f", b - a }')s"
    }
    say "-- ffmpeg, 300 frames each"
    bench() {   # bench <tag> <ffmpeg args...>
        local tag=$1; shift
        step_begin
        FF 120 -benchmark "$@" -f null - > "$T/ff-perf-$tag.log" 2>&1
        local rc=$?
        hung $rc "ffmpeg $tag" && exit 1
        say "   $tag: $(grep -o 'utime=.*' "$T/ff-perf-$tag.log" || echo "FAILED (rc $rc)")$(grep -qE 'reject|LOST|rror' "$T/ff-perf-$tag.log" && echo ' !! errors')"
        step_end "$tag" 300
    }
    bench vcn-decode -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/hevc_2160.mp4" -map 0:v
    bench vcn-decode+copy -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/hevc_2160.mp4" -map 0:v -vf hwdownload,format=nv12
    # the copy through FFmpeg's pooled staging buffers (+ memcpy) instead of importing each frame's memory (a kext
    # host import, map and unmap per frame)
    bench vcn-decode+copy-staging -init_hw_device vulkan=vk:0,avoid_host_import=1 -hwaccel vulkan -hwaccel_device vk \
        -hwaccel_output_format vulkan -i "$T/hevc_2160.mp4" -map 0:v -vf hwdownload,format=nv12
    bench cpu-decode -i "$T/hevc_2160.mp4" -map 0:v
    bench vcn10-decode+copy -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/hevc10_2160.mp4" -map 0:v -vf hwdownload,format=p010le
    bench vcn10-copy+yuv420p -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/hevc10_2160.mp4" -map 0:v -vf hwdownload,format=p010le,format=yuv420p
    say "-- ffplay, 10 s each, with 4 s of sample profile from second 4"
    profile() {   # profile <tag> <ffplay args...>: PLAY in the background, sample the ffplay process meanwhile
        local tag=$1; shift
        step_begin
        PLAY "$tag" "$@" &
        local job=$! pid="" i
        # ffplay itself: time and env carry its path among their arguments, ffplay's own command line starts with it
        for i in 1 2 3 4 5 6 7 8 9 10; do pid=$(pgrep -n -f "^$T/ff/bin/ffplay"); [ -n "$pid" ] && break; sleep 0.5; done
        if [ -n "$pid" ]; then
            sleep 4
            # round 56's first sample came back with an empty call graph: then once more as root
            sample "$pid" 4 -mayDie -file "$T/sample-$tag.log" >/dev/null 2>&1
            grep -q 'Thread_' "$T/sample-$tag.log" 2>/dev/null ||
                sudo -n sample "$pid" 3 -mayDie -file "$T/sample-$tag.log" >/dev/null 2>&1
            [ -s "$T/sample-$tag.log" ] && say "   sample: $(grep -m1 -E '^(Analysis of sampling|Sampling process)' "$T/sample-$tag.log")" || say "   sample: none taken"
        else say "   no ffplay process to sample"; fi
        wait $job || exit 1
        step_end "ffplay $tag" 300
    }
    profile vcn $VCNPLAY "$T/hevc_2160.mp4"
    profile vcn-import -hwaccel vulkan "$T/hevc_2160.mp4"
    profile cpu "$T/hevc_2160.mp4"
    profile vcn10 $VCNPLAY "$T/hevc10_2160.mp4"
    exit 0
fi

if [ $MODE = play ]; then
    PLAY play $VCNPLAY "$@" "$PLAY"
    exit 0
fi

say "== 2. VCN against the CPU decoder, frame by frame"
ok=0; bad=0
for c in h264_1080 h264_2160 hevc_2160 hevc10_2160 user_noaudio; do
    src=$VID/$c.mp4; [ -f "$src" ] || { say "$c: no $src"; continue; }
    fmt=nv12; [ $c = hevc10_2160 ] && fmt=p010le
    cp "$src" "$T/$c.mp4"
    say "-- $c ($fmt)"
    # first 30 frames with FFmpeg's and vkshim's word on the device (-v verbose), into its own log
    FF 60 -v verbose -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/$c.mp4" -map 0:v -frames:v 30 \
        -vf hwdownload,format=$fmt -f null - > "$T/ff-$c-smoke.log" 2>&1
    rc=$?; hung $rc "$c (30 frames)"
    grep -E 'vkshim|selected|^\[Vulkan[^]]*\] +[0-9]+: |rror|ail|reject|lost|LOST|radv' "$T/ff-$c-smoke.log" | head -16 | tee -a "$LOG"
    if [ $rc != 0 ]; then say "   $c: VCN decode of 30 frames FAILED (rc $rc): $(tail -2 "$T/ff-$c-smoke.log" | tr '\n' ' ')"; bad=$((bad+1)); [ -n "$HUNG" ] && break; continue; fi
    FF 180 -v error -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/$c.mp4" -map 0:v -vf hwdownload,format=$fmt \
        -f framemd5 "$T/$c.hw.md5" > "$T/ff-$c-hw.log" 2>&1
    rc=$?; grep -v '^vkshim' "$T/ff-$c-hw.log" | head -5 | tee -a "$LOG"
    hung $rc "$c (all frames)" && { bad=$((bad+1)); break; }
    FF 180 -v error -i "$T/$c.mp4" -map 0:v -vf format=$fmt -f framemd5 "$T/$c.sw.md5" 2>&1 | head -5 | tee -a "$LOG"
    sums "$T/$c.hw.md5" > "$T/$c.hw"; sums "$T/$c.sw.md5" > "$T/$c.sw"
    nh=$(wc -l < "$T/$c.hw" | tr -d ' '); ns=$(wc -l < "$T/$c.sw" | tr -d ' ')
    ref="no Linux reference"
    if [ -f "$VID/ref/$c.md5" ]; then cmp -s "$T/$c.hw" "$VID/ref/$c.md5" && ref="= Linux" || ref="DIFFERENT from Linux"; fi
    if [ "$nh" -gt 0 ] && cmp -s "$T/$c.hw" "$T/$c.sw"; then
        say "   $c: $nh frames, VCN = CPU decoder (identical), $ref"; ok=$((ok+1))
    else
        first=$(cmp "$T/$c.hw" "$T/$c.sw" 2>/dev/null | sed -n 's/.*line \([0-9]*\).*/\1/p')
        say "   $c: VCN $nh frames, CPU $ns frames: DIFFERENT${first:+ from frame $((first-1))}, $ref"; bad=$((bad+1))
    fi
    rm -f "$T/$c.hw.md5" "$T/$c.sw.md5"
done
say "== identical: $ok, failed or different: $bad"
[ -n "$HUNG" ] && exit 1

say "== 3. CPU time of the decode alone (utime+stime is the CPU's work, rtime the wall clock)"
for c in h264_2160 hevc_2160 hevc10_2160; do
    [ -f "$T/$c.mp4" ] || continue
    FF 120 -benchmark -hwaccel vulkan -hwaccel_output_format vulkan -i "$T/$c.mp4" -map 0:v -f null - > "$T/ff-$c-bench.log" 2>&1
    rc=$?; hw=$(grep -o 'utime=.*' "$T/ff-$c-bench.log")
    hung $rc "$c (VCN benchmark)" && exit 1
    # -f null takes whatever the decoder hands over: a time without its errors would pass for a working decode
    err=$(grep -c -E 'reject|DEVICE_LOST|failed to decode|rror' "$T/ff-$c-bench.log")
    sw=$(FF 120 -benchmark -i "$T/$c.mp4" -map 0:v -f null - 2>&1 | grep -o 'utime=.*')
    say "   $c 300 frames: VCN ${hw:-FAILED (rc $rc)}$([ "$err" != 0 ] && echo " !! $err error lines, the time means nothing") | CPU ${sw:-FAILED}"
done

say "== 4. ffplay: 4K on VCN, then the same on the CPU (10 s each), 30 s of the clip with sound; look at the picture"
for c in hevc_2160 h264_2160 hevc10_2160; do
    [ -f "$T/$c.mp4" ] || continue
    PLAY $c $VCNPLAY "$T/$c.mp4" || exit 1
done
[ -f "$T/hevc_2160.mp4" ] && { PLAY hevc_2160-cpu "$T/hevc_2160.mp4" || exit 1; }
[ -f "$VID/user_av.mp4" ] && PLAY user_av $VCNPLAY -t 30 "$VID/user_av.mp4"
