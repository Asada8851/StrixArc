#!/bin/bash
# boot_bench.sh: round 60's boot-time pieces on Linux - phxboot.sh (a new RADV on trial, VCN at boot), update_radv.sh,
# vplay - with stand-ins for macOS's tools and the GPU, and every absolute path moved under the work dir.
#   tools/bench/boot_bench.sh [linux ffplay] [clips dir] [work dir]
# phxboot runs as on a boot with PhxAMDFB on the panel (M2): bring-up (full.sh), claim, arming, the 90 s WindowServer
# watch (the stand-in sleep returns at once), then VCN. The stand-ins log what would have changed the machine
# (shutdown, nvram, mesboot) to $B/actions.log.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
FPL=${1:-$HOME/opt/nullarc-cross/ffmpeg/build-linux-ffplay/ffplay}
CLIPS=${2:-}
B=${3:-$R/build/tests/boot}
fails=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); cat "$B/G/phxboot.log" 2>/dev/null | tail -15; fi; }

setup() {   # a fresh "machine": /Users/Shared/gpu, GPUBundles, logs, the fakes
    rm -rf "$B"; mkdir -p "$B/fake" "$B/G/phxctl" "$B/G/fw" "$B/G/logs" "$B/GB/nvmtl" "$B/DR" "$B/db" "$B/state" "$B/BK"
    : > "$B/actions.log"
    echo "old radv" > "$B/old.dylib"; echo "new radv" > "$B/GB/nvmtl/libvulkan_radeon.dylib"
    echo vcnfw > "$B/G/fw/vcn_4_0_5.bin"; touch "$B/G/phxboot-m2"
    sed -e "s|/Users/Shared/gpu|$B/G|g; s|/Users/Shared/nullarc-backup|$B/BK|g; s|/var/log/phxboot.log|$B/G/phxboot.log|g" \
        -e "s|/var/db/phxboot.pending|$B/db/phxboot.pending|g; s|/Library/GPUBundles|$B/GB|g" \
        -e "s|/Library/Logs/DiagnosticReports|$B/DR|g; s|export HOME=/Users/Shared|export HOME=$B|" \
        "$R/tools/phxctl/phxboot.sh" > "$B/phxboot.sh"
    cd "$B/fake"
    cat > ioreg <<EOF
#!/bin/bash
case "\$*" in
  *IOFramebuffer*) echo '+-o PhxAMDFB  <class PhxAMDFB>';;
  *PhxAMDFB*-d2*) echo '+-o PhxAMDFB  <class PhxAMDFB>';;
  *PhxAMDFB*) echo '+-o PhxAMDFB  <class PhxAMDFB>';;
  *NMXAccel*) echo '+-o NMXAccel  <class NMXAccel>';;
  *PhxBringup*) echo '  "phx-build" = 44'; [ -e $B/state/adopted ] && echo '  "phx-vcn" = 1' || echo '  "phx-vcn" = 0';;
  -l*) echo '  "phx-build" = 44'; echo '  "phx-chip" = "strix-point"';;
esac
EOF
    cat > sysctl <<'EOF'
#!/bin/bash
case "$*" in *nvaccel_iop_ok*) echo 1234;; *phxvcn_jobs*) echo -1;; esac; exit 0
EOF
    printf '#!/bin/bash\necho "nvram $*" >> %s/actions.log\n' "$B" > nvram
    printf '#!/bin/bash\necho "shutdown $*" >> %s/actions.log\n' "$B" > shutdown
    printf '#!/bin/bash\ncase "$*" in *WindowServer*) [ "$1" = -x ] && echo 4242; exit 0;; esac; exit 1\n' > pgrep
    printf '#!/bin/bash\nexit 0\n' > codesign; cp codesign chown; cp codesign xattr
    # sleep: at once; "sleep 15" (the hang watcher's loop) ends the watcher
    printf '#!/bin/bash\n[ "$1" = 15 ] && exit 1; exit 0\n' > sleep
    printf '#!/bin/bash\necho 0\n' > id
    chmod +x ./*
    printf '#!/bin/bash\necho "compute: up"; echo "gfx: up"\n' > "$B/G/full.sh"
    cat > "$B/G/phxctl/mesboot" <<EOF
#!/bin/bash
echo "mesboot \$*" >> $B/actions.log
case "\$1 \$2" in
  "vcn up") [ -n "\${FAKE_VCN_HANG:-}" ] && { echo "hang"; kill -9 \$PPID; exit 9; }
            [ -n "\${FAKE_VCN_FAIL:-}" ] && { echo "vcn: PSP LOAD_IP_FW failed"; exit 1; }; echo "vcn: up";;
  "vcn adopt") touch $B/state/adopted; echo "vcn: adopt -> 0x0";;
  "vcn down") rm -f $B/state/adopted; echo "vcn: stopped";;
esac
exit 0
EOF
    chmod +x "$B/G/phxctl/mesboot"
    cd "$R"
}
boot() { PATH=$B/fake:$PATH timeout 60 bash "$B/phxboot.sh"; echo "rc=$?" >> "$B/actions.log"; }
radv_now() { cat "$B/GB/nvmtl/libvulkan_radeon.dylib"; }

echo "## 1. a new RADV on trial, WindowServer fine; VCN at boot"
setup; printf 'new\n%s\n' "$B/old.dylib" > "$B/G/radv.pending"; touch "$B/G/phxboot-vcn"
boot
check "trial started and the new RADV kept" "grep -q 'new RADV on trial this boot' $B/G/phxboot.log && grep -q 'the new RADV kept' $B/G/phxboot.log && [ ! -e $B/G/radv.pending ] && [ \"\$(radv_now)\" = 'new radv' ]"
check "VCN up, ring test, adopted; vcn.pending gone" "grep -q 'mesboot vcn up' $B/actions.log && grep -q 'mesboot vcn ringtest 3' $B/actions.log && grep -q 'mesboot vcn adopt' $B/actions.log && grep -q 'VCN up and handed to PhxBringup' $B/G/phxboot.log && sleep 0.3 && [ ! -e $B/G/vcn.pending ]"
check "no reboot, M2 armed for the next boot, mark cleared" "! grep -q '^shutdown' $B/actions.log && grep -q 'nvram phxfb-boot=1' $B/actions.log && [ ! -e $B/db/phxboot.pending ]"

echo "## 2. the last boot ran the new RADV and never finished (mark left): the previous one back, no kill switch"
setup; printf 'tried\n%s\n' "$B/old.dylib" > "$B/G/radv.pending"; touch "$B/db/phxboot.pending"
boot
check "previous RADV back, pending gone" "grep -q 'did not get WindowServer through its watch' $B/G/phxboot.log && [ \"\$(radv_now)\" = 'old radv' ] && [ ! -e $B/G/radv.pending ]"
check "no kill switch, the boot goes on with the old RADV" "[ ! -e $B/G/phxboot-off ] && grep -q 'WindowServer pid 4242' $B/G/phxboot.log"

echo "## 3. WindowServer crashes 3 times on the new RADV: the previous one back, M2 kept, reboot"
setup; printf 'new\n%s\n' "$B/old.dylib" > "$B/G/radv.pending"
# the crash reports appear while WindowServer is on the GPU: the stand-in sleep drops them in during the watch
cat > "$B/fake/sleep" <<EOF
#!/bin/bash
[ "\$1" = 15 ] && exit 1
[ "\$1" = 5 ] && for i in 1 2 3; do touch "$B/DR/WindowServer-crash\$i.ips"; done
exit 0
EOF
boot
check "RADV back, M2 kept and armed, rebooted" "grep -q 'M2 failed (WindowServer crashed or hung 3 times on the GPU) on a new RADV' $B/G/phxboot.log && [ \"\$(radv_now)\" = 'old radv' ] && [ -e $B/G/phxboot-m2 ] && grep -q 'nvram phxfb-boot=1' $B/actions.log && grep -q 'shutdown -r now' $B/actions.log"

echo "## 4. the last boot hung bringing VCN up (vcn.pending left): VCN at boot off"
setup; touch "$B/G/phxboot-vcn" "$B/G/vcn.pending" "$B/db/phxboot.pending"
boot
check "flag removed, no VCN this boot, no kill switch" "grep -q 'hung within 20 s of bringing VCN up' $B/G/phxboot.log && [ ! -e $B/G/phxboot-vcn ] && ! grep -q 'mesboot vcn' $B/actions.log && [ ! -e $B/G/phxboot-off ]"

echo "## 5. VCN fails to come up: VCN down, flag off, the desktop as before"
setup; touch "$B/G/phxboot-vcn"
FAKE_VCN_FAIL=1 boot
check "failure logged, vcn down, flag removed, boot finished" "grep -q 'VCN at boot failed' $B/G/phxboot.log && grep -q 'mesboot vcn down' $B/actions.log && [ ! -e $B/G/phxboot-vcn ] && grep -q '^done' $B/G/phxboot.log"

echo "## 6. no RADV on trial, no VCN flag: as before this round"
setup
boot
check "no RADV or VCN lines, no mesboot vcn" "! grep -qE 'RADV|VCN' $B/G/phxboot.log && ! grep -q 'mesboot vcn' $B/actions.log && grep -q '^done' $B/G/phxboot.log"

echo "## 6b. an orderly shutdown (SIGTERM) during the watch on the new RADV: on trial again at the next boot"
setup; printf 'new\n%s\n' "$B/old.dylib" > "$B/G/radv.pending"
printf '#!/bin/bash\n[ "$1" = 15 ] && exit 1\n[ "$1" = 5 ] && exec /usr/bin/sleep 1\nexit 0\n' > "$B/fake/sleep"
PATH=$B/fake:$PATH bash "$B/phxboot.sh" & pb=$!
for i in $(seq 1 50); do grep -q 'new RADV on trial' "$B/G/phxboot.log" 2>/dev/null && break; /usr/bin/sleep 0.1; done
/usr/bin/sleep 1.5; kill -TERM $pb; wait $pb 2>/dev/null
check "trap: pending back to 'new', the backup line kept, the new RADV still installed" "grep -q 'shutdown: marks cleared' $B/G/phxboot.log && [ \"\$(head -1 $B/G/radv.pending)\" = new ] && [ \"\$(sed -n 2p $B/G/radv.pending)\" = $B/old.dylib ] && [ \"\$(radv_now)\" = 'new radv' ]"

echo "## 7. update_radv.sh: install, then --undo"
setup; mkdir -p "$B/pkg/nvmtl"; echo "newest radv" > "$B/pkg/nvmtl/libvulkan_radeon.dylib"; echo "installed radv" > "$B/GB/nvmtl/libvulkan_radeon.dylib"
sed -e "s|/Users/Shared/gpu|$B/G|g; s|/Users/Shared/nullarc-backup|$B/BK|g; s|/Library/GPUBundles|$B/GB|g" \
    "$R/tools/phxctl/update_radv_strix.sh" > "$B/pkg/update_radv.sh"
PATH=$B/fake:$PATH bash "$B/pkg/update_radv.sh" > "$B/u.txt" 2>&1
check "installed, backup made, pending 'new' with the backup's path" "[ \"\$(radv_now)\" = 'newest radv' ] && [ \"\$(head -1 $B/G/radv.pending)\" = new ] && [ \"\$(cat \"\$(sed -n 2p $B/G/radv.pending)\")\" = 'installed radv' ]"
PATH=$B/fake:$PATH bash "$B/pkg/update_radv.sh" --undo > "$B/u.txt" 2>&1
check "--undo: the installed one back, pending gone" "[ \"\$(radv_now)\" = 'installed radv' ] && [ ! -e $B/G/radv.pending ]"

if [ -x "$FPL" ] && [ -n "$CLIPS" ] && [ -f "$CLIPS/hevc_2160.mp4" ]; then
    echo "## 8. vplay: VCN up (phx-vcn 1) -> VCN; VCN down -> CPU"
    setup; mkdir -p "$B/G/ffmpeg/bin" "$B/G/ffmpeg/lib" "$B/home"
    cc -O2 -shared -fPIC -o "$B/G/ffmpeg/lib/libvulkan.so.1" "$R/tools/vkshim/vkshim.c" -ldl
    { echo '#!/bin/bash'; echo 'export LD_LIBRARY_PATH=$DYLD_LIBRARY_PATH'
      echo "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy exec $FPL \"\$@\""; } > "$B/G/ffmpeg/bin/ffplay"; chmod +x "$B/G/ffmpeg/bin/ffplay"
    sed -e "s|/Users/Shared/gpu|$B/G|g" "$R/tools/phxctl/vplay" > "$B/vplay"
    touch "$B/state/adopted"
    HOME=$B/home PATH=$B/fake:$PATH bash "$B/vplay" "$CLIPS/hevc_2160.mp4" -autoexit -t 3 > /dev/null 2>&1
    check "VCN up: logged as VCN, NV12" "tail -1 $B/home/Library/Logs/vplay.log | grep -q 'hevc_2160.mp4: VCN (NV12 frames), texture NV12'"
    rm -f "$B/state/adopted"
    HOME=$B/home PATH=$B/fake:$PATH bash "$B/vplay" "$CLIPS/hevc_2160.mp4" -autoexit -t 3 > /dev/null 2>&1
    check "VCN down: logged as CPU, IYUV" "tail -1 $B/home/Library/Logs/vplay.log | grep -q 'CPU (VCN not up this boot), texture IYUV'"
    cat "$B/home/Library/Logs/vplay.log"

    echo "## 9. vcn_boot_check.sh after such a boot (VCN held, kept RADV, the player installed)"
    touch "$B/state/adopted"; mkdir -p "$B/stick/nullarc/gpu/nvmtl" "$B/home/gpu/metal" "$B/app/VCN Player.app"
    ln -sf "$CLIPS" "$B/stick/nullarc/video"; cp "$B/GB/nvmtl/libvulkan_radeon.dylib" "$B/stick/nullarc/gpu/nvmtl/"
    printf '#!/bin/bash\necho "results: 4096/4096 correct"\n' > "$B/home/gpu/metal/mtlprobe"; chmod +x "$B/home/gpu/metal/mtlprobe"
    printf '#!/bin/bash\necho "[fake nullarc_logs] tag=$1"\n' > "$B/stick/nullarc/gpu/nullarc_logs.sh"
    echo "VCN up and handed to PhxBringup: video decode for clients" >> "$B/G/phxboot.log"
    echo "vcn: adopt -> 0x0" > "$B/G/logs/vcn-boot.txt"
    printf '#!/bin/bash\necho 15.8.1\n' > "$B/fake/sw_vers"; chmod +x "$B/fake/sw_vers"; rm -f "$B/fake/id"   # (update_radv wanted root; the check refuses it)
    sed -e "s|/Users/Shared/gpu|$B/G|g; s|/Users/Shared/nullarc-backup|$B/BK|g; s|/Library/GPUBundles|$B/GB|g" \
        -e "s|/var/log/phxboot.log|$B/G/phxboot.log|g; s|/usr/local/bin/vplay|$B/vplay|g; s|/Applications/VCN Player.app|$B/app/VCN Player.app|g" \
        "$R/tools/phxctl/vcn_boot_check.sh" > "$B/stick/nullarc/gpu/vcn_boot_check.sh"
    chmod +x "$B/vplay"
    HOME=$B/home PATH=$B/fake:$PATH bash "$B/stick/nullarc/gpu/vcn_boot_check.sh" < /dev/null > "$B/vbc.txt" 2>&1
    grep -E '^   |^==' "$B/vbc.txt" | head -30
    check "boot check: VCN held, RADV kept, 3 Metal runs, 3 vplay runs on VCN, idle numbers" "grep -q 'held by PhxBringup since boot: yes' $B/vbc.txt && grep -q 'not on trial (kept)' $B/vbc.txt && [ \$(grep -c 'mtlprobe .*: rc 0' $B/vbc.txt) = 3 ] && [ \$(grep -c 'vplay.log: .*VCN (NV12 frames)' $B/vbc.txt) = 3 ] && grep -q 'MMHUB flushes (mirrored GPU mappings)' $B/vbc.txt"
fi
echo "boot_bench: $([ $fails = 0 ] && echo PASS || echo "FAILED ($fails)")"
exit $fails
