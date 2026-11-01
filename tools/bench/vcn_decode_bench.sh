#!/bin/bash
# vcn_decode_bench.sh: runs tools/phxctl/vcn_decode.sh on Linux the way the Mac would, before a hardware round.
#   tools/bench/vcn_decode_bench.sh <linux ffmpeg> <linux ffplay> <clips dir> [work dir]
# <linux ffmpeg>/<linux ffplay>: built for Linux from the same source and ffmpeg-patches/ with Vulkan (h264/hevc/aac
# decoders and hwaccels, mov, framemd5/null, SDL2 for ffplay); <clips dir>: the stick's video/ (h264_1080 h264_2160
# hevc_2160 hevc10_2160 user_noaudio user_av .mp4).
# The stand-ins: mesboot, sudo (never runs shutdown), ioreg, sw_vers, codesign, xattr, sysctl, dmesg, killall; FFmpeg
# and ffplay are the Linux ones (ffplay on SDL's offscreen video and dummy audio) with vkshim (tools/vkshim) as their
# libvulkan.so.1 and this machine's RADV as "the round's RADV".
# Cases: the whole round; up/adopt failing; a busy release; a broken RADV; a decode that hangs; errors in the benchmark;
# ffplay hanging; play mode. Each prints the lines that matter; the checks that must hold are marked PASS/FAIL.
set -u
FFL=$(readlink -f "${1:?usage: vcn_decode_bench.sh <linux ffmpeg> <linux ffplay> <clips dir> [work dir]}")
FPL=$(readlink -f "${2:?linux ffplay}")
CLIPS=$(cd "${3:?clips dir}" && pwd)
R=$(cd "$(dirname "$0")/../.." && pwd)
B=${4:-$R/build/tests/vcn_decode}
RADV=${RADV:-/usr/lib/libvulkan_radeon.so}
rm -rf "$B"; mkdir -p "$B/fake" "$B/home/gpu" "$B/ref"
fails=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi; }

cc -O2 -Wall -Wextra -Werror -shared -fPIC -I/usr/include -o "$B/libvulkan.so.1" "$R/tools/vkshim/vkshim.c" -ldl || exit 1
# Linux references: the CPU decoder's checksums (what the stick's video/ref holds)
for c in h264_1080 h264_2160 hevc_2160 hevc10_2160 user_noaudio; do
    fmt=nv12; [ $c = hevc10_2160 ] && fmt=p010le
    "$FFL" -y -v error -i "$CLIPS/$c.mp4" -map 0:v -vf format=$fmt -f framemd5 "$B/$c.md5" &&
        grep -v '^#' "$B/$c.md5" | awk -F, '{gsub(/ /, "", $NF); print $NF}' > "$B/ref/$c.md5"
done

cd "$B/fake"
cat > sudo <<'EOF'
#!/bin/bash
case "$*" in *shutdown*) echo "[fake sudo] $*" >> "$BENCH/sudo.log"; exit 0;; esac
while [ "${1:-}" = -n ] || [ "${1:-}" = -v ]; do [ "$1" = -v ] && exit 0; shift; done
[ $# = 0 ] && exit 0
"$@"
EOF
cat > ioreg <<'EOF'
#!/bin/bash
case "$*" in *NMXAccel*) echo '+-o NMXAccel  <class NMXAccel>';; *PhxBringup*) echo '  "phx-build" = 43';; esac
EOF
printf '#!/bin/bash\necho 15.8.1\n' > sw_vers
printf '#!/bin/bash\nexit 0\n' > codesign; cp codesign xattr
printf '#!/bin/bash\ncase "$*" in *phxvcn*) echo 0;; *) /usr/bin/sysctl "$@";; esac\n' > sysctl
printf '#!/bin/bash\necho "PhxBringup: adopted VCN ring (fake)"\n' > dmesg
printf '#!/bin/bash\necho "[fake killall] $*" >> "$BENCH/sudo.log"; exit 0\n' > killall
cat > sample <<'EOF'
#!/bin/bash
# macOS sample(1) stand-in: checks it got a live process and writes a header where -file says
pid=$1; f=""; while [ $# -gt 0 ]; do [ "$1" = -file ] && f=$2; shift; done
kill -0 "$pid" 2>/dev/null || exit 1
echo "Sampling process $pid ($(tr '\0' ' ' < /proc/$pid/cmdline | cut -c1-60)) for 4 seconds" > "$f"
echo "    2237 Thread_1   DispatchQueue_1: com.apple.main-thread  (serial)" >> "$f"
EOF
chmod +x ./*
cd "$B"

# a stick: gpu/ with the script, gpu/vcn (mesboot stand-in, firmware, RADV, stats), gpu/ffmpeg, video/ with ref
stick() {   # stick <dir> [ffmpeg wrapper prelude]
    local G=$1/nullarc/gpu
    mkdir -p "$G/vcn" "$G/ffmpeg/bin" "$G/ffmpeg/lib" "$1/nullarc/video"
    cp "$R/tools/phxctl/vcn_decode.sh" "$G/"
    printf '#!/bin/bash\necho "[fake nullarc_logs] tag=$1 files: $(shift; for f; do [ -f "$f" ] && echo -n "$(basename $f) "; done)"\n' > "$G/nullarc_logs.sh"
    cat > "$G/vcn/mesboot" <<'EOF'
#!/bin/bash
echo "vcn: $2 (fake)"; echo "$*" >> "$BENCH/mesboot.log"
[ "$2" = release ] && [ -n "${FAKE_BUSY:-}" ] && { echo "vcn: release -> 0x2d5 (jobs still running)"; exit 1; }
[ "$2" = "${FAKE_FAIL:-none}" ] && exit 1
exit 0
EOF
    chmod +x "$G/vcn/mesboot"; echo fw > "$G/vcn/vcn_4_0_5.bin"; cp "$RADV" "$G/vcn/libvulkan_radeon.dylib"
    cp "$B/libvulkan.so.1" "$G/ffmpeg/lib/"; echo "macOS name" > "$G/ffmpeg/lib/libvulkan.dylib"
    { echo '#!/bin/bash'; echo "${2:-}"; echo 'export LD_LIBRARY_PATH=$DYLD_LIBRARY_PATH'; echo "exec $FFL \"\$@\""; } > "$G/ffmpeg/bin/ffmpeg"
    { echo '#!/bin/bash'; echo '[ -n "${FAKE_PLAY_HANG:-}" ] && exit 142'; echo 'export LD_LIBRARY_PATH=$DYLD_LIBRARY_PATH'
      echo "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy exec -a \"\$0\" $FPL \"\$@\""; } > "$G/ffmpeg/bin/ffplay"
    chmod +x "$G/ffmpeg/bin/ffmpeg" "$G/ffmpeg/bin/ffplay"
    for f in "$CLIPS"/*.mp4; do ln -sf "$f" "$1/nullarc/video/"; done
    cp -r "$B/ref" "$1/nullarc/video/"
}
run() {   # run <stick> [args...]: the script as go.sh would, the output in $B/out.txt
    local s=$1; shift; : > "$B/mesboot.log"
    BENCH=$B HOME=$B/home PATH=$B/fake:$PATH bash "$s/nullarc/gpu/vcn_decode.sh" "$@" < /dev/null > "$B/out.txt" 2>&1
    MB=$(tr '\n' ';' < "$B/mesboot.log")
}

echo "## the whole round"
stick "$B/s1"; run "$B/s1"
grep -E '^   |== identical' "$B/out.txt"
check "5 clips identical, = Linux" "[ \$(grep -c 'VCN = CPU decoder (identical), = Linux' $B/out.txt) = 5 ]"
check "benchmark without error lines" "! grep -q 'error lines' $B/out.txt"
grep -E 'ffplay [a-z_0-9-]+: exit' "$B/out.txt"
check "ffplay on VCN: NV12 textures, nothing dropped" "[ \$(grep -cE 'ffplay (hevc_2160|h264_2160|user_av): exit 0, texture NV12, dropped 0' $B/out.txt) = 3 ]"
check "ffplay on the CPU: IYUV" "grep -qE 'ffplay hevc_2160-cpu: exit 0, texture IYUV, dropped 0' $B/out.txt"
check "VCN up, ringtest, adopt, release, down" "echo '$MB' | grep -q 'up .*ringtest 3;vcn adopt .*;vcn release;vcn down;'"

echo "## perf"
run "$B/s1" perf
grep -E '^   ' "$B/out.txt"
check "perf: 6 ffmpeg measurements" "[ \$(grep -cE '^   (vcn-decode|vcn-decode\+copy|vcn-decode\+copy-staging|cpu-decode|vcn10-decode\+copy|vcn10-copy\+yuv420p): utime=' $B/out.txt) = 6 ]"
check "perf: 4 ffplay runs sampled (ffplay itself)" "[ \$(grep -c 'sample: Sampling process .*ff/bin/ffplay' $B/out.txt) = 4 ]"
check "perf: VCN runs show NV12, CPU run IYUV" "grep -q 'ffplay vcn: exit 0, texture NV12' $B/out.txt && grep -q 'ffplay vcn-import: exit 0, texture NV12' $B/out.txt && grep -q 'ffplay cpu: exit 0, texture IYUV' $B/out.txt"
check "perf: flush counts per step" "[ \$(grep -c 'MMHUB flushes' $B/out.txt) -ge 8 ]"
check "perf: VCN released and down" "echo '$MB' | grep -q 'release;vcn down;'"

echo "## a Linux reference that differs"
sed -i '5s/^./0/' "$B/s1/nullarc/video/ref/h264_1080.md5"; run "$B/s1"
check "h264_1080 flagged DIFFERENT from Linux" "grep -q 'h264_1080: 300 frames, VCN = CPU decoder (identical), DIFFERENT from Linux' $B/out.txt"

echo "## adopt fails / up fails"
FAKE_FAIL=adopt run "$B/s1"; check "adopt fails: down, no release" "echo '$MB' | grep -q 'adopt .*;vcn down;$' && ! echo '$MB' | grep -q release"
FAKE_FAIL=up run "$B/s1"; check "up fails: STOP and down" "grep -q 'STOP: vcn up failed' $B/out.txt && echo '$MB' | grep -q 'vcn down;$'"

echo "## play with a busy release"
FAKE_BUSY=1 run "$B/s1" play "$CLIPS/user_noaudio.mp4" -t 3
check "busy release: VCN left up, no down" "grep -q 'VCN left up' $B/out.txt && ! echo '$MB' | grep -q down"
check "play: options passed, VCN frames shown" "grep -q -- '\$ ffplay -hwaccel vulkan -hwaccel_opts avoid_host_import=1 -t 3' $B/out.txt && grep -q 'ffplay play: exit 0, texture NV12' $B/out.txt"

echo "## a broken RADV"
stick "$B/s2"; cp /usr/lib/libc.so.6 "$B/s2/nullarc/gpu/vcn/libvulkan_radeon.dylib"; run "$B/s2"
check "every clip FAILED, round goes on to release/down" "[ \$(grep -c 'VCN decode of 30 frames FAILED' $B/out.txt) = 5 ] && echo '$MB' | grep -q 'release;vcn down;'"

echo "## errors during the benchmark"
stick "$B/s3" 'case "$*" in *-benchmark*hwaccel*) echo "radv/amdgpu: The CS has been rejected, see dmesg for more information (-22)." >&2;; esac'
run "$B/s3"; grep -E '^   h|^   hevc' "$B/out.txt" | grep VCN | grep utime
check "benchmark errors flagged" "[ \$(grep -c 'error lines, the time means nothing' $B/out.txt) = 3 ]"

echo "## ffplay hangs"
FAKE_PLAY_HANG=1 run "$B/s1"
check "ffplay timeout stops the round, VCN released" "grep -q 'ffplay hevc_2160 hit its time limit' $B/out.txt && [ \$(grep -c '^\\\$ ffplay' $B/out.txt) = 1 ] && echo '$MB' | grep -q 'release;vcn down;'"

echo "## a decode that hangs (60 s)"
stick "$B/s4" 'case "$*" in *hwaccel*) sleep 100;; esac'; run "$B/s4"
check "hang stops the round after the first clip" "grep -q 'h264_1080 (30 frames) hit its time limit' $B/out.txt && ! grep -q -- '-- h264_2160' $B/out.txt && echo '$MB' | grep -q 'release;vcn down;'"

check "no temp dirs left" "! ls -d /tmp/vcndec.* >/dev/null 2>&1"
echo "vcn_decode_bench: $([ $fails = 0 ] && echo PASS || echo "FAILED ($fails)")"
exit $fails
