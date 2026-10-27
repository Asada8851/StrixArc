#!/bin/bash
# Once per boot (LaunchDaemon local.phxboot): GPU bring-up and the Metal accelerator claim; when PhxAMDFB took the panel
# (M2) also the display arming, before WindowServer opens the panel. PhxAMDFB holds the GPU busy from boot until this
# script releases it (or its 120 s timer does), so GPUWrangler publishes the GPU with its accelerator and WindowServer
# composites on the 780M from its first start. Fail-safes: a boot whose previous attempt never finished skips everything,
# turns itself off and leaves PhxAMDFB off (the marker survives a hang); WindowServer crashing 3 times in 90 s is taken
# off the GPU; NVRAM phxfb-boot is armed again only after a good M2 boot and only while $G/phxboot-m2 exists.
# Kill switch: the file $G/phxboot-off.
G=/Users/Shared/gpu; export HOME=/Users/Shared
LOG=/var/log/phxboot.log; MARK=/var/db/phxboot.pending; OFF=$G/phxboot-off
ALLOW=/Library/GPUBundles/nvmtl-allow.txt; DR=/Library/Logs/DiagnosticReports
exec >>"$LOG" 2>&1
echo "== $(date '+%F %T') phxboot"
# Logs of this boot so far and of the one before it (a hang or a watchdog reboot leaves its story in the unified log),
# in $G/logs; the USB stick is not mounted this early, so nullarc_logs.sh copies them there on its next run. Late and in
# the background, so the bring-up and WindowServer are not held up; the last 6 kept (2026-10-26: build 34/35's crashes
# left nothing to read).
[ -f $G/nullarc_logs.sh ] && ( sleep 100; NULLARC_LOGS_DEST=$G/logs bash $G/nullarc_logs.sh atboot
    ls -1dt $G/logs/*-atboot 2>/dev/null | tail -n +7 | while IFS= read -r d; do rm -rf "$d"; done ) >/dev/null 2>&1 &
release() { sysctl -w debug.phxfb_hold=0 >/dev/null 2>&1; }
apps_only() { printf '# phxboot: Metal for the built-in test tools only\n' > $ALLOW; }
# every process gets the Metal device; WindowServer too once the accelerator is armed (NullMoth's own default list).
# Denied ("-name", before "*"): the background neural-network users. mediaanalysisd (Photos analysis, which starts by
# itself on an idle Mac on power) ran Espresso convolution kernels and hung the GPU within seconds (2026-10-21), and
# Spotlight and photoanalysisd load the same MPS/Espresso kernels; on the CPU they only take longer.
# iconservicesagent was denied once too, for flat yellow/green icons; that was the plugin's storage-texture aliasing
# (fixed 2026-10-21).
DENY="mediaanalysisd photoanalysisd Spotlight"
all_apps() { { printf '# phxboot: Metal for every app%s\n' "$1"; for d in $DENY; do printf -- '-%s\n' "$d"; done
               printf '*\n'; [ -z "$2" ] || printf '%s\n' "$2"; } > $ALLOW; }
# PhxAMDFB without the arming leaves the panel black: give up on M2 (until $G/phxboot-m2 is put back) and reboot into
# the firmware framebuffer
m2_fail() {
    echo "M2 failed ($1): PhxAMDFB off, rebooting"
    apps_only; sysctl -w debug.phxagdc=0 >/dev/null 2>&1
    nvram -d phxfb-boot; rm -f "$MARK" $G/phxboot-m2; sync; shutdown -r now; exit 0
}
# PhxAMDFB comes from the Aux KC, which loads well after PhxBringup: wait for the panel's framebuffer (PhxAMDFB, or
# IONDRV when PhxAMDFB was not armed) before telling which one took it
wait_fb() { for i in $(seq 1 90); do ioreg -rc IOFramebuffer -d0 | grep -q 'class ' && break; sleep 1; done; }
if [ -e "$OFF" ]; then echo "kill switch present: nothing to do"; release; exit 0; fi
# The boot before turned SDMA copies on (as WindowServer got the panel) and never got a minute past it: the hang is put
# on SDMA alone - SDMA copies off, the bring-up mark cleared so no kill switch, WindowServer stays on the GPU (build 38
# hung WindowServer's display pipe that way, 2026-10-26 22:11)
if [ -e $G/hz120.pending ]; then
    echo "the last boot did not get through the switch to 120 Hz: 120 Hz at boot off ($G/phxboot-120 removed)"
    rm -f $G/hz120.pending $G/phxboot-120 "$MARK"; sync
fi
if [ -e $G/gpucopy.pending ]; then
    echo "the last boot hung within a minute of turning SDMA copies on: SDMA copies off ($G/phxboot-gpucopy removed)"
    rm -f $G/gpucopy.pending $G/phxboot-gpucopy "$MARK"; sync
fi
if [ -e "$MARK" ]; then
    echo "previous attempt did not finish: phxboot and PhxAMDFB off"
    touch "$OFF"; rm -f "$MARK"; nvram -d phxfb-boot; sync
    wait_fb; ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && { echo "PhxAMDFB took the panel: rebooting"; shutdown -r now; }
    release; exit 0
fi
for i in $(seq 1 60); do ioreg -l | grep -q '"phx-build"' && break; sleep 1; done
wait_fb
M2=0; ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && M2=1
CHIP=$(ioreg -l | sed -n 's/.*"phx-chip" = "\(.*\)".*/\1/p' | head -1); echo "chip ${CHIP:-?}"
ioreg -l | grep -q '"phx-build"' || { [ $M2 = 1 ] && m2_fail "PhxBringup not loaded"; echo "PhxBringup not loaded"; exit 0; }
echo "PhxAMDFB on the panel: $M2"
touch "$MARK"; sync
# An orderly shutdown (launchd's SIGTERM) is not a hang: drop the marks, so a reboot asked for during the bring-up or the
# watches does not set the kill switch at the next boot (go.sh rebooted 23:23 mid-watch, 2026-10-26). A real hang or a
# forced power-off sends nothing and leaves them.
trap 'echo "$(date "+%T") shutdown: marks cleared"; rm -f "$MARK" $G/gpucopy.pending $G/hz120.pending; sync; exit 0' TERM
apps_only
MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash $G/full.sh kiqsched | grep -E "^compute:|^gfx:|adopt"
$G/phxctl/mesboot claim
# no BO cache flushing per submit/wait (the GPU maps system memory snooped): 41 fps window drag instead of 13, and no
# artifacts seen (2026-10-20)
sysctl -w debug.phxbos_flush=0 >/dev/null 2>&1
for i in $(seq 1 20); do ioreg -rc NMXAccel -d0 | grep -q NMXAccel && break; sleep 1; done
if ! ioreg -rc NMXAccel -d0 | grep -q NMXAccel; then
    [ $M2 = 1 ] && m2_fail "NMXAccel did not start"
    echo "NMXAccel did not start"; release; rm -f "$MARK"; exit 0
fi
if [ $M2 = 1 ]; then
    sysctl -w debug.nvaccelfb=1 >/dev/null; sleep 1
    sysctl -w debug.nvaccel_iop=1 >/dev/null; sysctl -w debug.nvaccelfb=3 >/dev/null; sleep 1
    sysctl -w debug.phxagdc=1 >/dev/null; sleep 1
    # frames copied with SSE streaming loads (XMM saved around them): 1.0 ms a frame against 2.1 (2026-10-20)
    sysctl -w debug.nvaccel_iop_sse=1 >/dev/null
    # a copied frame completes on the next VBL tick, as a flip latches: fewer frames presented twice within one refresh
    # and then a skipped one (2026-10-21)
    sysctl -w debug.nvaccel_iop_vsync=1 >/dev/null 2>&1
    all_apps "; WindowServer composites on the GPU (armed only)" '!WindowServer'
    # 120 Hz ($G/phxboot-120): the panel switched while WindowServer is still held back (mesboot hz 120: the link
    # retrained at 8.1 Gb/s through DMCUB, 2026-10-27), then PhxAMDFB measures it again, so the mode and its VBL tick say
    # 120 Hz when WindowServer opens the panel. Failed: back to 60 Hz and 120 Hz off at boot; the way back failed too
    # (the panel dark): reboot - the firmware lights it at 60 Hz. $G/hz120.pending stays until WindowServer's 90 s watch
    # below is over: a hang, or the power button held on a dark panel, turns 120 Hz off for the next boot.
    if [ -e $G/phxboot-120 ]; then
        mkdir -p $G/logs; touch $G/hz120.pending; sync
        HZ_QUIET=1 $G/phxctl/mesboot hz 120 > $G/logs/hz120-boot.txt 2>&1; rc=$?
        if [ $rc = 0 ]; then
            sysctl -w debug.phxfb_remeasure=1 >/dev/null 2>&1
            echo "panel at 120 Hz ($(grep '^hz: done' $G/logs/hz120-boot.txt | cut -c5-)); PhxAMDFB reports $(sysctl -n debug.phxfb_remeasure 2>/dev/null) mHz"
        else
            echo "120 Hz did not come up (rc $rc, $G/logs/hz120-boot.txt): back to 60 Hz, 120 Hz at boot off"
            rm -f $G/phxboot-120
            HZ_QUIET=1 $G/phxctl/mesboot hz 60 >> $G/logs/hz120-boot.txt 2>&1; rc=$?
            sysctl -w debug.phxfb_remeasure=1 >/dev/null 2>&1
            if [ $rc != 0 ]; then
                echo "back to 60 Hz failed too (rc $rc): rebooting"; rm -f $G/hz120.pending "$MARK"; sync; shutdown -r now; exit 0
            fi
        fi
    fi
    # WindowServer's process starts early but opens the panel only once GPUWrangler publishes the GPU, which the busy
    # hold delays. A panel opened before the arming (the hold timed out) stays black, and a restarted WindowServer then
    # asserts in CoreDisplay_CreateDisplayForCGXDisplayDevice and hangs: no way back but a reboot.
    ioreg -rc PhxAMDFB -d2 | grep -q IOFramebufferUserClient && m2_fail "WindowServer opened the panel before the arming"
    # crash reports and watchdog spins (WindowServer_..._userspace_watchdog_timeout.spin) both count
    before=$(ls $DR | grep -c '^WindowServer[-_]')
    release
    echo "armed and released at $(date '+%T'); agdc $(sysctl -n debug.phxagdc)"
    # Frames copied by SDMA into a second buffer the display flips to (PhxBringup 36+), once fps_check.sh passed
    # ($G/phxboot-gpucopy): from WindowServer's first frame now (it used to wait ~2.5 min, after the watch below,
    # 2026-10-27). $G/gpucopy.pending stays for a minute: a hang inside it makes the next boot switch SDMA copies off.
    if [ -e $G/phxboot-gpucopy ]; then
        touch $G/gpucopy.pending; sync
        sysctl -w debug.nvaccel_iop_gpucopy=1 >/dev/null 2>&1 && echo "frames copied by SDMA, flipped by the display"
        ( sleep 60
          echo "SDMA after a minute: $(sysctl -n debug.phxscanout_copies 2>/dev/null) frames; state: $(sysctl -n debug.phxscanout_state 2>/dev/null); dropped by the pixel check: $(sysctl -n debug.phxscanout_mismatch 2>/dev/null)"
          rm -f $G/gpucopy.pending; sync ) &
    fi
    for ((t = 0; t < 90; t += 5)); do
        sleep 5 & wait $!   # wait, not sleep: a SIGTERM runs the trap at once
        n=$(( $(ls $DR | grep -c '^WindowServer[-_]') - before ))
        [ $n -ge 3 ] && m2_fail "WindowServer crashed or hung $n times on the GPU"
    done
    pgrep -qx WindowServer || m2_fail "no WindowServer"
    echo "WindowServer pid $(pgrep -x WindowServer), frames on the panel $(sysctl -n debug.nvaccel_iop_ok)"
    rm -f $G/hz120.pending; sync
else
    release
    # Strix Point is new: Metal for the test tools only until the user opts every app in with $G/phxboot-apps
    if [ "$CHIP" = strix-point ] && [ ! -e $G/phxboot-apps ]; then
        apps_only; echo "Strix Point: Metal for the test tools only (touch $G/phxboot-apps for every app)"
    else
        all_apps " (WindowServer stays on the firmware framebuffer)" ""
    fi
fi
[ -e $G/phxboot-m2 ] && nvram phxfb-boot=1 && echo "PhxAMDFB armed for the next boot"
rm -f "$MARK"; sync
echo "done $(date '+%T')"

# A GPU hang leaves WindowServer waiting on a fence until watchdogd kills it, and every WindowServer after it hangs the
# same way: the session is logged out and the screen stays black. There is no GPU reset yet, so two WindowServer
# reports within 5 minutes reboot the Mac; three such reboots within an hour take WindowServer off the GPU.
watch_hangs() {
    local base n now first=0
    base=$(ls $DR | grep -c '^WindowServer-.*\.ips$')
    while sleep 15; do
        n=$(ls $DR | grep -c '^WindowServer-.*\.ips$'); [ $n -gt $base ] || continue
        now=$(date +%s); [ $first = 0 ] && first=$now
        if [ $((n - base)) -ge 2 ]; then
            echo "$(date '+%F %T') WindowServer hung $((n - base)) times on the GPU: rebooting"
            echo $now >> $G/hang-reboots
            if [ $(awk -v t=$((now - 3600)) '$1 > t' $G/hang-reboots | wc -l) -ge 3 ]; then
                echo "3 GPU hangs within an hour: WindowServer goes back to the firmware framebuffer"
                rm -f $G/phxboot-m2; nvram -d phxfb-boot
            fi
            sync; ${PHX_REBOOT:-shutdown -r now}; return
        fi
        [ $((now - first)) -gt 300 ] && { base=$n; first=0; }
    done
}
[ $M2 = 1 ] && watch_hangs
