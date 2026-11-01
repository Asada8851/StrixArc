#!/bin/bash
# radv_check_bench.sh: tools/phxctl/radv_check.sh on Linux with stand-ins for macOS's tools and for mtlprobe/yuvprobe,
# which answer like the real ones and write the Metal plugin's "driver opened directly" line for the RADV
# NVMTL_ICD_LIB names (or the installed one). Cases: both RADVs pass; the new one fails a test; the new one hangs one.
#   tools/bench/radv_check_bench.sh [work dir]
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
B=${1:-$R/build/tests/radv_check}
rm -rf "$B"; mkdir -p "$B/fake" "$B/home/gpu/metal" "$B/stick/nullarc/gpu/vcn" "$B/stick/nullarc/metal"
fails=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi; }
cd "$B/fake"
printf '#!/bin/bash\ncase "$*" in *NMXAccel*) echo "+-o NMXAccel  <class NMXAccel>";; esac\n' > ioreg
printf '#!/bin/bash\necho 15.8.1\n' > sw_vers
printf '#!/bin/bash\nexit 0\n' > codesign; cp codesign xattr
command -v shasum >/dev/null || printf '#!/bin/bash\nsha1sum "$@"\n' > shasum
chmod +x ./*
# the probes: what radv_check greps for, and the plugin's line; FAKE_NEW_FAIL=<tag> / FAKE_NEW_HANG=<tag> break the new RADV
cat > "$B/probe" <<'EOF'
#!/bin/bash
tag=${1:-compute}; [ "$tag" = quick ] && tag=yuv
lib=${NVMTL_ICD_LIB:-/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib}
echo "vk: driver opened directly: $lib (image test)" > "$NVMTL_LOG_PATH"
if [ -n "${NVMTL_ICD_LIB:-}" ]; then
    [ "${FAKE_NEW_HANG:-}" = "$tag" ] && sleep 100
    [ "${FAKE_NEW_FAIL:-}" = "$tag" ] && { echo "stress: FAIL blit: 12 of 4096 words wrong"; exit 33; }
fi
echo "results: 4096/4096 correct ($tag)"; exit 0
EOF
chmod +x "$B/probe"; cp "$B/probe" "$B/home/gpu/metal/mtlprobe"; cp "$B/probe" "$B/stick/nullarc/metal/yuvprobe"
G=$B/stick/nullarc/gpu
cp "$R/tools/phxctl/radv_check.sh" "$G/"
printf '#!/bin/bash\necho "[fake nullarc_logs] tag=$1"\n' > "$G/nullarc_logs.sh"
echo "new radv" > "$G/vcn/libvulkan_radeon.dylib"; echo "installed radv" > "$B/installed.dylib"
run() { HOME=$B/home PATH=$B/fake:$PATH RADV_CHECK_INSTALLED=$B/installed.dylib bash "$G/radv_check.sh" < /dev/null > "$B/out.txt" 2>&1; }

echo "## both pass"
run; grep -E '^   |^==' "$B/out.txt"
check "8 runs, the plugin opened the right RADV each time" "[ \$(grep -c 'installed: rc 0, RADV /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib' $B/out.txt) = 4 ] && [ \$(grep -c 'new: rc 0, RADV /tmp/radvchk' $B/out.txt) = 4 ]"
check "verdict: passes" "grep -q 'the new RADV passes what the installed one passes' $B/out.txt"
echo "## the new RADV fails stress"
FAKE_NEW_FAIL=stress run
check "stress flagged DIFFERENT, verdict not installed" "grep -q 'stress: installed rc 0, new rc 33  <-- DIFFERENT' $B/out.txt && grep -q 'DIFFERENCES above' $B/out.txt"
echo "## the new RADV hangs render (60 s)"
FAKE_NEW_HANG=render run
check "render hang: time limit, DIFFERENT" "grep -q 'render / new hit its time limit' $B/out.txt && grep -q 'render: installed rc 0, new rc 142' $B/out.txt"
check "no temp dirs left" "! ls -d /tmp/radvchk.* >/dev/null 2>&1"
echo "radv_check_bench: $([ $fails = 0 ] && echo PASS || echo "FAILED ($fails)")"
exit $fails
