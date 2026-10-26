#!/bin/bash
# Strix Point (880M), macOS 15: bring the GPU up at every boot (LaunchDaemon local.phxboot), as nullarc does on the
# 780M. The Metal plugin and PhxAccel are already in place from setup_metal.sh; this adds only the boot-time part.
#   sudo bash install_boot.sh                    the GPU comes up at boot; Metal for the test tools only
#   sudo bash install_boot.sh --apps             the same, and every app gets Metal on the GPU (WindowServer stays as it is)
#   sudo bash install_boot.sh --undo             remove the LaunchDaemon (the GPU stays off at boot)
# WindowServer on the GPU (M2), in two steps:
#   sudo bash install_boot.sh --ws-trial         ONE boot with WindowServer on the GPU. Needs try15.sh passed on this
#                                                setup ($G/ws-ready). Arms NVRAM phxfb-boot once and nothing else: the
#                                                boot after the trial is back on the firmware framebuffer whatever happens
#                                                (black screen, hang, panic: hold the power button, boot again).
#                                                After the trial boot: bash ~/gpu/ws_check.sh
#   sudo bash install_boot.sh --windowserver     WindowServer on the GPU at every boot. Needs a trial ws_check.sh passed
#                                                ($G/ws-trial-ok); phxboot re-arms each boot only after a good one.
#   sudo bash install_boot.sh --no-windowserver  back to the firmware framebuffer from the next boot
#   (--force skips the try15/ws_check requirement; it does not skip the other checks)
# Run it after `bash install.sh` (it takes the scripts, tools and firmware from ~/gpu). Takes effect at the next boot;
# /var/log/phxboot.log then ends with "done". Kill switch: sudo touch /Users/Shared/gpu/phxboot-off
set -eu
G=/Users/Shared/gpu; PL=/Library/LaunchDaemons/local.phxboot.plist
U=${SUDO_USER:-$(stat -f %Su /dev/console)}; UG=$(eval echo "~$U")/gpu
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=boot; FORCE=0
for a in "$@"; do
    case $a in
        --undo|--apps|--ws-trial|--windowserver|--no-windowserver) MODE=${a#--} ;;
        --force) FORCE=1 ;;
        *) echo "unknown option $a"; exit 1 ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
if [ $MODE = undo ]; then
    launchctl bootout system "$PL" 2>/dev/null || true
    rm -f "$PL" "$G/phxboot-m2"; nvram -d phxfb-boot 2>/dev/null || true
    echo "LaunchDaemon removed: the GPU is no longer brought up at boot ($G is left in place)"; exit 0
fi
if [ $MODE = no-windowserver ]; then
    rm -f "$G/phxboot-m2"; nvram -d phxfb-boot 2>/dev/null || true
    echo "WindowServer goes back to the firmware framebuffer from the next boot (the GPU still comes up for Metal)"; exit 0
fi
for f in phxboot.sh full.sh bringup5.sh phxctl/phxctl phxctl/mesboot phxctl/fwplan; do
    [ -e "$UG/$f" ] || { echo "missing $UG/$f: run 'bash install.sh' from this package first"; exit 1; }
done
[ "$(ls "$UG"/fw/*.bin 2>/dev/null | wc -l | tr -d ' ')" -ge 9 ] || { echo "firmware in $UG/fw is incomplete"; exit 1; }
[ -f /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver ] || { echo "Metal plugin not installed: run setup_metal.sh first"; exit 1; }
ioreg -l | grep -q '"phx-chip" = "strix-point"' || { echo "PhxBringup (Strix Point) is not loaded"; exit 1; }

if [ $MODE = ws-trial ] || [ $MODE = windowserver ]; then
    B=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
    [ "${B:-0}" -ge 33 ] || { echo "STOP: PhxBringup build ${B:-?} is loaded; WindowServer on the GPU needs 33 (the USB stick's EFI)"; exit 1; }
    [ -f "$PL" ] || { echo "STOP: install the boot-time bring-up first (sudo bash install_boot.sh), reboot, run try15.sh"; exit 1; }
    [ ! -e "$G/phxboot-off" ] || { echo "STOP: $G/phxboot-off is set (a boot did not finish the bring-up): look at /var/log/phxboot.log"; exit 1; }
    if [ $FORCE = 0 ]; then
        [ -f "$G/ws-ready" ] || { echo "STOP: try15.sh has not passed on this setup (no $G/ws-ready): bash ~/gpu/try15.sh"; exit 1; }
        echo "try15: $(cat "$G/ws-ready")"
        if [ $MODE = windowserver ] && [ ! -f "$G/ws-trial-ok" ]; then
            echo "STOP: no passed trial yet: sudo bash install_boot.sh --ws-trial, reboot, then bash ~/gpu/ws_check.sh"; exit 1
        fi
    fi
    # the login window comes up on the GPU too, and may stay black there: log in by itself (FileVault logs in at its
    # pre-boot screen, which the firmware draws)
    AL=$(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null || true)
    if [ -z "$AL" ] && ! fdesetup status 2>/dev/null | grep -q 'On'; then
        echo "STOP: turn on automatic login first (System Settings -> Users & Groups -> Automatically log in as)"; exit 1
    fi
fi

# the boot-time pieces, fresh from ~/gpu (also on a WindowServer step: phxboot.sh and the tools may be newer)
mkdir -p "$G/phxctl" "$G/fw" "$G/metal"
for f in phxboot.sh full.sh bringup5.sh; do install -m 755 "$UG/$f" "$G/$f"; done
[ -f "$UG/nullarc_logs.sh" ] && install -m 755 "$UG/nullarc_logs.sh" "$G/nullarc_logs.sh"
install -m 755 "$UG/phxctl/phxctl" "$UG/phxctl/mesboot" "$UG/phxctl/fwplan" "$G/phxctl/"
install -m 644 "$UG"/fw/*.bin "$G/fw/"
[ -f "$UG/metal/mtlprobe" ] && install -m 755 "$UG/metal/mtlprobe" "$G/metal/"
chown -R root:wheel "$G"
install -m 644 -o root -g wheel "$HERE/gpu/local.phxboot.plist" "$PL"

case $MODE in
boot|apps)
    rm -f "$G/phxboot-off" /var/db/phxboot.pending "$G/phxboot-m2" "$G/ws-ready" "$G/ws-trial" "$G/ws-trial-ok"
    nvram -d phxfb-boot 2>/dev/null || true
    if [ $MODE = apps ]; then touch "$G/phxboot-apps"; echo "every app will get Metal on the 880M"
    else rm -f "$G/phxboot-apps"; echo "Metal for the test tools only (re-run with --apps for every app)"; fi
    echo "installed: $PL -> $G/phxboot.sh. Reboot; then: tail /var/log/phxboot.log (ends with 'done'), then bash ~/gpu/try15.sh"
    ;;
ws-trial)
    # one arming, no re-arming: phxboot arms the next boot only while $G/phxboot-m2 exists
    rm -f "$G/phxboot-m2" "$G/ws-trial-ok"
    nvram phxfb-boot=1
    nvram phxfb-boot >/dev/null 2>&1 || { echo "STOP: NVRAM phxfb-boot did not stick"; exit 1; }
    date '+%F %T' > "$G/ws-trial"
    cat <<'T'
Armed for ONE boot: WindowServer on the 880M. Reboot now. What to expect:
  - the boot looks as usual; the desktop can take up to ~2 minutes (PhxAMDFB holds the GPU until phxboot has it up);
  - after login, run:  bash ~/gpu/ws_check.sh   (it says whether the desktop is drawn by the GPU, and what to do next);
  - black screen or a hang for more than 3 minutes: hold the power button and boot again. That boot is back on the
    firmware framebuffer by itself (the arming is used up at the trial boot's start). If it also brought the GPU
    up only partly, phxboot turns itself off once (/Users/Shared/gpu/phxboot-off); ws_check.sh tells.
  - last resort, from Linux: add -phxfboff to boot-args in the USB stick's EFI/OC/config.plist.
T
    ;;
windowserver)
    touch "$G/phxboot-m2"
    nvram phxfb-boot=1
    nvram phxfb-boot >/dev/null 2>&1 || { echo "STOP: NVRAM phxfb-boot did not stick"; exit 1; }
    echo "WindowServer on the 880M from the next boot on; phxboot re-arms each boot only after a good one."
    echo "Back to the firmware framebuffer: sudo bash install_boot.sh --no-windowserver"
    ;;
esac
