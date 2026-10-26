#!/bin/bash
# Strix Point (880M) panel: what amdgpu's display code (DC, DCN 3.5) does to change the internal panel's mode, for
# nullarc's display work under macOS (native 3072x1920, then 120 Hz). Runs in the normal Linux desktop (GNOME on
# Wayland): no reboot, no blacklist. As your user, not root (the mode switches go through your session's
# org.gnome.Mutter.DisplayConfig); it asks for the sudo password once, for tracefs.
#
# Four switches, each its own phase in the trace (trace_marker "nullarc phase ..."), ~4 s apart:
#   1. 3072x1920@120 -> 3072x1920@60     pixel clock halved (DISPCLK/DTO, maybe the eDP link), same timing totals
#   2. 3072x1920@60  -> 3072x1920@120    and back
#   3. 3072x1920@120 -> 1920x1200@60     the plane scaled up to the panel: what the firmware leaves macOS today
#   4. back to the mode you had          (also done by the cleanup if anything stops the script early)
# The screen goes dark briefly at each switch. Switches are "temporary" Mutter configs: your saved settings stay.
#
# Recorded: every amdgpu register write and read (amdgpu_device_wreg/rreg), DC's own events (amdgpu_dm: pipe and
# clock state, the writes DC makes), and kprobes on what DC hands to others: SMU messages (dcn35_smu_send_msg_with_param
# and the DISPCLK/DPPCLK setters), the DMUB command queue (first 64 bytes of each command: DCN 3.5's DMCUB firmware
# programs part of the display itself, invisible as register writes), and eDP link training (lane count, link rate).
# drm.debug KMS+DP for the duration (DPCD/AUX traffic of the link training) is streamed from the kernel log.
# Output: ~/strix_modeswitch_trace.txt.zst, ~/strix_modeswitch_dmesg.txt, ~/strix_modeswitch_state.txt
set -u
[ "$(id -u)" != 0 ] || { echo "run it as your user, not with sudo (it asks for the password itself)"; exit 1; }
T=/sys/kernel/tracing
OUT=~/strix_modeswitch; STATE=$OUT"_state.txt"
R() { sudo tee "$1" > /dev/null; }   # echo VALUE | R FILE: a root write

# the session's display configuration: modes of the built-in panel, and switching it (temporary configs)
mutter() {   # mutter list | mutter apply <mode-id>
python3 -I - "$@" <<'EOF'
import sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
def call(m, args=None, sig=None):
    return bus.call_sync("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                         "org.gnome.Mutter.DisplayConfig", m, args, None, 0, -1, None).unpack()
serial, monitors, logical, _ = call("GetCurrentState")
mon = next((m for m in monitors if m[2].get("is-builtin")), None) or monitors[0]
conn = mon[0][0]
if sys.argv[1] == "list":
    for mid, w, h, rate, _, _, p in mon[1]:
        if p.get("refresh-rate-mode") == "variable":
            continue
        print(mid, w, h, "%.3f" % rate, "current" if p.get("is-current") else "-", conn)
    sys.exit(0)
lm = next(l for l in logical if any(s[0] == conn for s in l[5]))
x, y, scale, transform, primary = lm[:5]
mode = sys.argv[2]
ok = [m for m in mon[1] if m[0] == mode]
if not ok:
    sys.exit("no mode " + mode)
if scale not in ok[0][5]:
    scale = ok[0][4]   # that mode's preferred scale
cfg = [(x, y, scale, transform, primary, [(conn, mode, {})])]
call("ApplyMonitorsConfig", GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (serial, 1, cfg, {})))
print("applied", mode, "scale", scale)
EOF
}

echo "== modes of the built-in panel"
mutter list > "$STATE.modes" || { echo "STOP: cannot reach org.gnome.Mutter.DisplayConfig (GNOME session?)"; exit 1; }
cat "$STATE.modes"
ORIG=$(awk '$5 == "current" {print $1}' "$STATE.modes")
M120=$(awk '$2 == 3072 && $3 == 1920 && $4 > 119 && $4 < 121 {print $1; exit}' "$STATE.modes")
M60=$(awk '$2 == 3072 && $3 == 1920 && $4 > 59 && $4 < 61 {print $1; exit}' "$STATE.modes")
M1200=$(awk '$2 == 1920 && $3 == 1200 && $4 > 59 && $4 < 61 {print $1; exit}' "$STATE.modes")
for v in ORIG M120 M60 M1200; do [ -n "${!v}" ] || { echo "STOP: no $v mode in the list above"; exit 1; }; done
echo "now $ORIG; switches: $M60, $M120, $M1200, back to $ORIG"
sudo -v || exit 1
[ -d $T/events/amdgpu ] || sudo mount -t tracefs nodev $T 2>/dev/null
sudo test -d $T/events/amdgpu || { echo "STOP: no amdgpu trace events in $T"; exit 1; }

# drm.debug is readable by root only; buffer_size_kb reads like "7 (expanded: 1408)": keep the number that applies
DBG0=$(sudo cat /sys/module/drm/parameters/debug); DBG0=${DBG0:-0}
BUF0=$(sudo cat $T/buffer_size_kb | sed -n 's/.*expanded: \([0-9]*\).*/\1/p;t;s/^\([0-9]*\).*/\1/p'); BUF0=${BUF0:-1408}
DMESGP=""
cleanup() {
    echo 0 | R $T/tracing_on
    for e in amdgpu/amdgpu_device_wreg amdgpu/amdgpu_device_rreg amdgpu_dm nullarc; do
        sudo test -e $T/events/$e/enable && echo 0 | R $T/events/$e/enable
    done
    sudo sh -c "grep -q nullarc $T/kprobe_events && echo > $T/kprobe_events" 2>/dev/null
    echo "$DBG0" | R /sys/module/drm/parameters/debug
    echo "$BUF0" | R $T/buffer_size_kb
    [ -n "$DMESGP" ] && sudo kill "$DMESGP" 2>/dev/null
    if [ -n "${ORIG:-}" ] && [ "$(mutter list | awk '$5 == "current" {print $1}')" != "$ORIG" ]; then
        mutter apply "$ORIG" > /dev/null && echo "display back to $ORIG"
    fi
}
trap cleanup EXIT INT TERM

echo "== trace setup"
echo 0 | R $T/tracing_on
echo 65536 | R $T/buffer_size_kb        # per CPU; a switch is tens of thousands of register accesses
echo 0 | R $T/options/overwrite         # if it fills, keep the start
echo | R $T/trace
echo 1 | R $T/events/amdgpu/amdgpu_device_wreg/enable
echo 1 | R $T/events/amdgpu/amdgpu_device_rreg/enable
if sudo test -d $T/events/amdgpu_dm; then
    echo 1 | R $T/events/amdgpu_dm/enable
    for e in amdgpu_dc_rreg dcn_fpu; do sudo test -e $T/events/amdgpu_dm/$e/enable && echo 0 | R $T/events/amdgpu_dm/$e/enable; done
fi
# kprobes: x86-64 arguments in %di, %si, %dx, %cx; each one is optional (an inlined or renamed function is skipped)
kp() { echo "$1" | sudo tee -a $T/kprobe_events > /dev/null 2>&1 && echo "   kprobe ${1%% *}" || echo "   (no kprobe for: ${1#* })"; }
C="c0=+0(%dx):x64 c1=+8(%dx):x64 c2=+16(%dx):x64 c3=+24(%dx):x64 c4=+32(%dx):x64 c5=+40(%dx):x64 c6=+48(%dx):x64 c7=+56(%dx):x64"
kp "p:nullarc/smu_msg dcn35_smu_send_msg_with_param msg=%si:u32 param=%dx:u32"
kp "p:nullarc/set_dispclk dcn35_smu_set_dispclk khz=%si:s32"
kp "p:nullarc/set_dppclk dcn35_smu_set_dppclk khz=%si:s32"
kp "p:nullarc/update_clocks dcn35_update_clocks safe_to_lower=%dx:u8"
kp "p:nullarc/dmub_cmds dc_dmub_srv_cmd_list_queue_execute count=%si:u32 $C"
kp "p:nullarc/link_train dp_perform_link_training lanes=+0(%dx):u32 rate=+4(%dx):u32"
kp "p:nullarc/link_train_retries perform_link_training_with_retries"
kp "p:nullarc/link_dpms_on link_set_dpms_on"
kp "p:nullarc/commit_streams dc_commit_streams"
sudo test -d $T/events/nullarc && echo 1 | R $T/events/nullarc/enable
echo 0x106 | R /sys/module/drm/parameters/debug      # DRIVER, KMS, DP: DC's logs and the AUX/DPCD traffic
sudo sh -c "dmesg -W > '$OUT""_dmesg.txt' 2>&1" & DMESGP=$!

phase() { echo "nullarc phase $1" | R $T/trace_marker; echo "== $1"; }
echo 1 | R $T/tracing_on
phase "1: $ORIG -> $M60";   mutter apply "$M60";   sleep 4
phase "2: $M60 -> $M120";   mutter apply "$M120";  sleep 4
phase "3: $M120 -> $M1200"; mutter apply "$M1200"; sleep 4
phase "4: $M1200 -> $ORIG"; mutter apply "$ORIG";  sleep 4
phase "end"
echo 0 | R $T/tracing_on

echo "== saving"
{ echo "# $(date '+%F %T') $(uname -r)"; cat "$STATE.modes"; echo "# /sys/class/drm eDP:";
  for c in /sys/class/drm/card*-eDP-*; do echo "$c $(cat $c/status) $(cat $c/enabled 2>/dev/null)"; done
  echo "# EDID (hex)"; for c in /sys/class/drm/card*-eDP-*; do xxd -p "$c/edid"; done
  echo "# kprobes"; sudo cat $T/kprobe_events; echo "# dropped events per CPU"; sudo sh -c "grep -H overrun $T/per_cpu/cpu*/stats" 2>/dev/null
} > "$STATE" 2>&1
rm -f "$STATE.modes"
sudo cat $T/trace | zstd -q -f -o "$OUT""_trace.txt.zst"
sleep 1; sudo chown "$(id -un)" "$OUT""_dmesg.txt"
echo "lines: $(zstd -dc "$OUT""_trace.txt.zst" | wc -l), phases: $(zstd -dc "$OUT""_trace.txt.zst" | grep -c 'nullarc phase'), register writes: $(zstd -dc "$OUT""_trace.txt.zst" | grep -c 'amdgpu_device_wreg')"
echo "dropped: $(grep -E 'overrun' "$STATE" | awk '{s += $NF} END {print s + 0}') events (0 is what we want)"
ls -la "$OUT"_trace.txt.zst "$OUT"_dmesg.txt "$STATE"
echo "done: tell Claude; nothing else to do (the display is back as it was)"
