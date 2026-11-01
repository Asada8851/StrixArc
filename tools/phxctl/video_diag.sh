#!/bin/bash
# video_diag.sh: where do video frames break on the 880M? Software decode only (no hardware decoder exists yet).
#  1. yuvprobe (tools/yuvprobe): biplanar IOSurfaces (420v/420f/x420) wrapped per plane, read back, converted to RGB,
#     written by the GPU, cycled through a decoder-style pool; frames VideoToolbox decodes from the test clips; an
#     AVPlayerLayer window (QuickTime's path without QuickTime).
#  2. audio (VIDEO_ONLY=audio, gpu/audio_check.sh): AVPlayer times a file with sound by the default output device's
#     clock, so a device that is published but never runs keeps the picture black. CoreAudio's devices, AppleHDA's
#     controllers/codecs/engines, then the same clips with and without their sound track.
#  3. players with software decode, whichever are installed: mpv --hwdec=no (--vo gpu, gpu-next, libmpv, and the
#     Vulkan context macvk), ffplay (SDL's Metal renderer, then OpenGL), IINA (libmpv), QuickTime Player, Safari. Each
#     plays a clip for ~8 s, then asks what was on the screen.
# Per run: the plugin's / VideoToolbox's / CoreMedia's lines from the unified log (log stream), the plugin's own
# /tmp/nvmtl.log lines, new kernel lines (PhxAccel), the display pipe's counters before and after, a screenshot, the
# busiest processes and new crash reports.
# Run: bash gpu/video_diag.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-video)
#   VIDEO_ONLY="audio probe mpv ffplay iina qt safari" picks parts; VIDEO_NOASK=1 asks nothing; VIDEO_TAG names the log folder.
set -u
[ "$(id -u)" != 0 ] || { echo "run it without sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=~/nullarc_video.txt; W=~/nullarc_video
rm -rf "$W"; mkdir -p "$W"; : > "$LOG"
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
LSP=""
finish() {
    [ -n "$LSP" ] && kill $LSP 2>/dev/null
    bash "$LOGS" "${VIDEO_TAG:-video}" "$LOG" "$W"/* | tail -1 | tee -a "$LOG"
}
trap finish EXIT
say() { echo "$*" | tee -a "$LOG"; }
want() { [ -z "${VIDEO_ONLY:-}" ] || case " $VIDEO_ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# yuvprobe and the clips: from the round folder (gpu/../metal, gpu/../video) into ~/gpu, so nothing plays off the stick
mkdir -p ~/gpu/metal ~/gpu/video
for d in "$HERE/../metal" ~/gpu/metal; do [ -f "$d/yuvprobe" ] && { cmp -s "$d/yuvprobe" ~/gpu/metal/yuvprobe || cp "$d/yuvprobe" ~/gpu/metal/; break; }; done
chmod +x ~/gpu/metal/yuvprobe 2>/dev/null; xattr -d com.apple.quarantine ~/gpu/metal/yuvprobe 2>/dev/null
YP=~/gpu/metal/yuvprobe; [ -x "$YP" ] || YP=""
if [ -d "$HERE/../video" ]; then
    for f in "$HERE"/../video/*.mp4; do cmp -s "$f" ~/gpu/video/"$(basename "$f")" || cp "$f" ~/gpu/video/; done
fi
V=~/gpu/video; C1=$V/h264_1080.mp4; C4=$V/h264_2160.mp4; H4=$V/hevc_2160.mp4; H10=$V/hevc10_2160.mp4
[ -f "$C1" ] || { say "STOP: no test clips in $V (the round folder's video/)"; exit 1; }

first() { local p; for p in "$@"; do [ -x "$p" ] && { echo "$p"; return; }; command -v "$p" 2>/dev/null && return; done; }
MPV=$(first mpv /usr/local/bin/mpv /opt/homebrew/bin/mpv /Applications/mpv.app/Contents/MacOS/mpv)
FFPLAY=$(first ffplay /usr/local/bin/ffplay /opt/homebrew/bin/ffplay)
IINA=""; [ -d /Applications/IINA.app ] && IINA=/Applications/IINA.app

say "== video_diag $(date '+%F %T'), macOS $(sw_vers -productVersion) $(sw_vers -buildVersion)"
say "PhxAMDFB on the panel: $(ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && echo yes || echo no), $(ioreg -rc PhxAMDFB -d1 | sed -n 's/.*"PhxFBRefreshMilliHz" = \([0-9]*\).*/\1/p' | head -1) mHz"
say "SDMA scan-out: $(sysctl -n debug.phxscanout_state 2>/dev/null), gpucopy $(sysctl -n debug.nvaccel_iop_gpucopy 2>/dev/null)"
say "allow-list: $(grep -v '^#' /Library/GPUBundles/nvmtl-allow.txt 2>/dev/null | tr '\n' ' ')"
say "plugin: $(shasum /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver 2>/dev/null | cut -c1-12), RADV $(shasum /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib 2>/dev/null | cut -c1-12)"
say "players: mpv ${MPV:-none}${MPV:+ ($("$MPV" --version 2>/dev/null | head -1))}; ffplay ${FFPLAY:-none}${FFPLAY:+ ($("$FFPLAY" -version 2>/dev/null | head -1 | cut -c1-40))}; IINA ${IINA:-none}; yuvprobe ${YP:-MISSING}"

K="nvaccel_iop_ok nvaccel_iop_fail nvaccel_iop_src_surf nvaccel_iop_src_res nvaccel_iop_blank nvaccel_iop_copy_skip
   nvaccel_iop_empty nvaccel_iop_gpu_copies nvaccel_iop_gpu_fail nvaccel_iop_gpu_busy phxscanout_copies phxscanout_mismatch
   phxscanout_moving phxscanout_busy phxscanout_gpulate"
snap() { local k; for k in $K; do printf '%s=%s ' $k "$(sysctl -n debug.$k 2>/dev/null || echo 0)"; done; }
delta() {   # what changed between two snaps
    local k a b out=""
    for k in $K; do a=$(echo "$1" | tr ' ' '\n' | sed -n "s/^$k=//p"); b=$(echo "$2" | tr ' ' '\n' | sed -n "s/^$k=//p")
        [ "${a:-0}" != "${b:-0}" ] && out="$out ${k#nvaccel_}+$(( ${b:-0} - ${a:-0} ))"; done
    echo "${out:- (no change)}"
}
PRED='eventMessage CONTAINS "NVMTL" OR eventMessage CONTAINS "NullMoth" OR eventMessage CONTAINS "NMXAccel"
  OR eventMessage CONTAINS "radv" OR subsystem BEGINSWITH "com.apple.videotoolbox" OR subsystem BEGINSWITH "com.apple.coremedia"
  OR subsystem BEGINSWITH "com.apple.avfoundation" OR process == "VTDecoderXPCService" OR process == "mpv" OR process == "ffplay"
  OR process == "yuvprobe" OR process == "coreaudiod" OR subsystem BEGINSWITH "com.apple.coreaudio"
  OR eventMessage CONTAINS "AppleHDA" OR eventMessage CONTAINS "AppleALC" OR ((process == "WindowServer" OR process == "QuickTime Player" OR process == "Safari"
  OR process BEGINSWITH "com.apple.WebKit" OR process == "IINA") AND (messageType == error OR messageType == fault))'
nvsize() { stat -f %z /tmp/nvmtl.log 2>/dev/null || echo 0; }
klast() { sudo -n dmesg 2>/dev/null | tail -1; }
TOUCH=$W/.since
cap() { local f=$1 n; n=$(wc -l < "$f"); [ "$n" -gt 4000 ] && { { head -3000 "$f"; echo "... ($((n - 4000)) lines left out) ..."; tail -1000 "$f"; } > "$f.t"; mv "$f.t" "$f"; }; }

begin_case() {   # name
    CASE=$1; say ""; say "=== $CASE"
    S0=$(snap); N0=$(nvsize); K0=$(klast); touch "$TOUCH"
    log stream --style compact --predicate "$PRED" > "$W/$CASE.logstream.txt" 2>&1 & LSP=$!
    sleep 1
}
shot() {   # a few seconds in: what WindowServer composited, downscaled
    screencapture -x -t jpg "$W/$CASE.jpg" 2>/dev/null && sips -Z 1600 "$W/$CASE.jpg" >/dev/null 2>&1 || say "   (no screenshot: Terminal needs Screen Recording in Privacy & Security)"
    top -l 2 -s 2 -o cpu -n 10 -stats pid,command,cpu,rsize 2>/dev/null | awk '/^PID/{n++} n==2' > "$W/$CASE.top.txt"
}
ASKED=""
ask() {
    [ -n "${VIDEO_NOASK:-}" ] && return
    printf '   что было на экране? 1 видео нормально, 2 чёрное/пустое/розовое окно, 3 артефакты (полосы, зелень, мусор),\n   4 рывки/замирания, 5 не открылось/упало. Цифра и, если хочешь, пару слов: '
    read -r a; say "   ответ: ${a:-<пусто>}"; ASKED="$ASKED
$CASE: ${a:-<пусто>}"
}
end_case() {   # rc
    kill $LSP 2>/dev/null; wait $LSP 2>/dev/null; LSP=""
    cap "$W/$CASE.logstream.txt"
    local n1; n1=$(nvsize); [ "$n1" -gt "$N0" ] && tail -c +$((N0 + 1)) /tmp/nvmtl.log > "$W/$CASE.nvmtl.txt"
    sudo -n dmesg 2>/dev/null | awk -v l="$K0" 'f; $0 == l {f = 1}' > "$W/$CASE.dmesg.txt"
    [ -s "$W/$CASE.dmesg.txt" ] || rm -f "$W/$CASE.dmesg.txt"
    local cr; cr=$(find ~/Library/Logs/DiagnosticReports /Library/Logs/DiagnosticReports -newer "$TOUCH" -type f 2>/dev/null)
    # report names carry spaces ("QuickTime Player-2026-...ips"): one per line
    echo "$cr" | while IFS= read -r f; do [ -n "$f" ] && cp "$f" "$W/" 2>/dev/null; done
    say "   exit $1; display pipe:$(delta "$S0" "$(snap)")"
    local L="$W/$CASE.logstream.txt"
    say "   plugin: $(grep -c NVMTL "$L") lines; formats it does not carry: $(grep -o 'pixel format [0-9]* is NOT carried' "$L" | sort -u | tr '\n' ' ')"
    say "   multi-plane surfaces: $(grep -c 'more than one plane' "$L"); NOT synced: $(grep -c 'NOT synced' "$L"); FAIL/fault lines: $(grep -E 'NVMTL|NullMoth' "$L" | grep -ciE 'fail|fault|refus')"
    say "   YUV planes on their own pages: $(grep -c 'plane [0-9]* of [0-9]* at +' "$L") (first lines only), private copies: $(grep -c 'more than one plane' "$L"), not importable: $(grep -c 'not importable' "$L")"
    say "   WindowServer's surface keep: $(grep 'WindowServer.*surface keep:' "$L" | tail -1 | sed 's/.*surface keep: //')"
    [ -n "$cr" ] && say "   crash reports: $(echo "$cr" | sed 's|.*/||' | tr '\n' ' ')"
    grep -E 'NVMTL|NullMoth' "$L" | grep -iE 'fail|fault|refus|NOT carried|NOT synced|no device|skipped' | sort | uniq -c | sort -rn | head -8 | sed 's/^/     /' | tee -a "$LOG"
}
# a command-line player: runs until it ends (at most secs + 10), screenshot at 4 s
cli_case() {   # name secs command...
    local name=$1 secs=$2; shift 2
    begin_case "$name"; say "   $*"
    "$@" < /dev/null > "$W/$name.out.txt" 2>&1 & local pid=$!
    sleep 4; shot
    local i=4; while kill -0 $pid 2>/dev/null && [ $i -lt $((secs + 10)) ]; do sleep 1; i=$((i + 1)); done
    if kill -0 $pid 2>/dev/null; then kill $pid; sleep 1; kill -9 $pid 2>/dev/null; echo "(killed after $i s)" >> "$W/$name.out.txt"; say "   still running after $i s: killed"; fi
    wait $pid 2>/dev/null; local rc=$?
    # yuvprobe play: the device it plays on and its first second, then how it ended
    local r; r=$(grep -E '^(INFO play|PASS|FAIL|GAP|HANG|yuvprobe:)' "$W/$name.out.txt" | cut -c1-300)
    { echo "$r" | head -2; echo "$r" | tail -3; } | awk 'NF && !seen[$0]++' | sed 's/^/   /' | tee -a "$LOG"
    ask; end_case $rc
}
# an app opened with a file: $START (if set) 2 s after the open, plays for secs, then the stop command ends it
START=""
app_case() {   # name secs "stop command" open-args...
    local name=$1 secs=$2 stop=$3; shift 3
    begin_case "$name"; say "   open $*"
    open "$@" > "$W/$name.out.txt" 2>&1; local rc=$?
    sleep 2; [ -n "$START" ] && eval "$START" >> "$W/$name.out.txt" 2>&1
    sleep 2; shot; sleep $((secs - 4 > 0 ? secs - 4 : 0))
    ask
    eval "$stop" >> "$W/$name.out.txt" 2>&1
    end_case $rc
}

# ---- audio
if want audio && [ -n "$YP" ]; then
    say ""; say "== audio: does a file with sound stay black because of the audio device?"
    "$YP" audio > "$W/audio-devices.txt" 2>&1; sed 's/^/   /' "$W/audio-devices.txt" | tee -a "$LOG"
    system_profiler SPAudioDataType > "$W/spaudio.txt" 2>&1
    ioreg -l -w0 -r -c AppleHDAController > "$W/ioreg-hdacontroller.txt" 2>&1
    ioreg -l -w0 -r -c IOHDACodecDevice > "$W/ioreg-hdacodec.txt" 2>&1
    ioreg -l -w0 -r -c IOAudioEngine > "$W/ioreg-audioengine.txt" 2>&1
    ioreg -l -w0 -r -c IOAudioDevice > "$W/ioreg-audiodevice.txt" 2>&1
    kextstat 2>/dev/null | grep -iE 'hda|alc|audio|lilu' > "$W/kexts-audio.txt"
    sudo -n dmesg 2>/dev/null | grep -iE 'hda|alc|audio' > "$W/dmesg-audio.txt"
    say "   AppleHDA controllers: $(grep -c '+-o AppleHDAController' "$W/ioreg-hdacontroller.txt"); codecs (vendor id): $(sed -n 's/.*"IOHDACodecVendorID" = \([0-9]*\).*/\1/p' "$W/ioreg-hdacodec.txt" | while read -r v; do printf '0x%08x ' "$v"; done)"
    say "   layout-id seen: $(grep -oE '"(alc-layout-id|layout-id)" = [^,}]*' "$W"/ioreg-*.txt | sed 's/.*= //' | sort -u | tr '\n' ' ')"
    say "   audio engines: $(grep -c '+-o' "$W/ioreg-audioengine.txt"), states: $(grep -oE '"IOAudioEngineState" = [0-9]+' "$W/ioreg-audioengine.txt" | sed 's/.*= //' | tr '\n' ' ')"
    say "   kexts: $(awk '{print $6}' "$W/kexts-audio.txt" | tr '\n' ' ')"
    sed 's/^/   /' "$W/spaudio.txt" | head -40 | tee -a "$LOG"
    UA=$V/user_av.mp4; UN=$V/user_noaudio.mp4; CA=$V/h264_1080_aac.mp4
    say ""; say "Окна с видео откроются по очереди (розовый фон = видео не нарисовалось). Ничего не трогай до вопроса."
    cli_case play-test-silent 10 "$YP" play "$C1" 10
    [ -f "$CA" ] && cli_case play-test-with-aac 10 "$YP" play "$CA" 10
    if [ -f "$UA" ]; then
        cli_case play-user-with-sound 10 "$YP" play "$UA" 10
        cli_case play-user-video-track-only 10 "$YP" play "$UA" 10 noaudio
    fi
    [ -f "$UN" ] && cli_case play-user-sound-removed 10 "$YP" play "$UN" 10
    say ""; say "QuickTime Player: если ролик не начал играть сам, нажми пробел в его окне."
    START='osascript -e "tell application \"QuickTime Player\" to play front document"'
    for c in "$UA" "$UN"; do [ -f "$c" ] && app_case "quicktime-$(basename "$c" .mp4)" 10 \
        'osascript -e "tell application \"QuickTime Player\" to quit" || pkill -x "QuickTime Player"' -a "QuickTime Player" "$c"; done
    START=""
fi

# ---- 1. yuvprobe
if want probe && [ -n "$YP" ]; then
    begin_case yuvprobe-synthetic
    NVMTL_ENABLE=1 "$YP" all > "$W/yuvprobe-synthetic.out.txt" 2>&1; rc=$?
    grep -E '^(FAIL|GAP|HANG)' "$W/yuvprobe-synthetic.out.txt" | head -20 | sed 's/^/   /' | tee -a "$LOG"
    tail -1 "$W/yuvprobe-synthetic.out.txt" | sed 's/^/   /' | tee -a "$LOG"
    end_case $rc
    begin_case yuvprobe-decoded
    { "$YP" vt
      for c in "$C1:60" "$C4:30" "$H4:30" "$H10:30"; do NVMTL_ENABLE=1 "$YP" asset "${c%:*}" "${c##*:}"; done; } > "$W/yuvprobe-decoded.out.txt" 2>&1; rc=$?
    grep -E '^(PASS|FAIL|HANG|INFO VideoToolbox|INFO .*own BGRA)' "$W/yuvprobe-decoded.out.txt" | cut -c1-260 | sed 's/^/   /' | tee -a "$LOG"
    end_case $rc
    say ""; say "Сейчас откроется окно с видео (розовый фон за ним - значит видео не нарисовалось). Ничего не трогай."
    cli_case yuvprobe-play-h264 8 env NVMTL_ENABLE=1 "$YP" play "$C1" 8
    cli_case yuvprobe-play-hevc4k 8 env NVMTL_ENABLE=1 "$YP" play "$H4" 8
    cli_case yuvprobe-play-hevc10 8 env NVMTL_ENABLE=1 "$YP" play "$H10" 8
fi

# ---- 2. players
if want qt; then
    say ""; say "QuickTime Player: если ролик не начал играть сам, нажми пробел в его окне."
    START='osascript -e "tell application \"QuickTime Player\" to play front document"'
    for c in "$C1" "$H4"; do
        app_case "quicktime-$(basename "$c" .mp4)" 10 \
            'osascript -e "tell application \"QuickTime Player\" to quit" || pkill -x "QuickTime Player"' -a "QuickTime Player" "$c"
    done
    START=""
fi
if want safari; then
    if pgrep -qx Safari; then
        say ""; say "Safari уже открыт: ролик откроется новой вкладкой, закрой её сам после вопроса."
        app_case safari-h264_1080 10 'true' -a Safari "$C1"
    else
        app_case safari-h264_1080 10 'osascript -e "tell application \"Safari\" to quit" || pkill -x Safari' -a Safari "$C1"
    fi
fi
if want mpv && [ -n "$MPV" ]; then
    m() { local name=$1 clip=$2; shift 2
          cli_case "$name" 8 "$MPV" --no-config --hwdec=no --length=8 --geometry=1280x720+120+120 --msg-level=all=v \
              --log-file="$W/$name.mpv.log" "$@" "$clip"; }
    m mpv-gpu "$C1" --vo=gpu
    m mpv-gpu-next "$C1" --vo=gpu-next
    m mpv-gpu-macvk "$C1" --vo=gpu --gpu-context=macvk
    m mpv-libmpv "$C1" --vo=libmpv   # the render API's vo: the command line has no host for it (IINA below is libmpv)
    m mpv-gpu-hevc4k "$H4" --vo=gpu
elif want mpv; then say ""; say "mpv: not installed (brew install mpv) - skipped"; fi
if want ffplay && [ -n "$FFPLAY" ]; then
    cli_case ffplay-metal 8 "$FFPLAY" -hide_banner -loglevel verbose -autoexit -t 8 -x 1280 -y 720 "$C1"
    cli_case ffplay-opengl 8 env SDL_RENDER_DRIVER=opengl "$FFPLAY" -hide_banner -loglevel verbose -autoexit -t 8 -x 1280 -y 720 "$C1"
elif want ffplay; then say ""; say "ffplay: not installed (brew install ffmpeg) - skipped"; fi
if want iina && [ -n "$IINA" ]; then
    app_case iina-h264_1080 10 'osascript -e "tell application \"IINA\" to quit" || pkill -x IINA' -a IINA "$C1"
fi

say ""; say "== summary"
grep -E '^(=== |   exit|   ответ|   (PASS|FAIL) )' "$LOG" | sed 's/^   /     /' > "$W/.sum"; tee -a "$LOG" < "$W/.sum"; rm -f "$W/.sum" "$TOUCH"
say "== done $(date '+%T')"
