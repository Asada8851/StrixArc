#!/bin/bash
# Strix Point (880M): every MMIO register read/write amdgpu does while bringing the GPU up, to diff against nullarc's
# bring-up (bringup5.sh + mesboot). Run as root on THIS laptop's Linux, booted once with the kernel argument
#     modprobe.blacklist=amdgpu
# so the driver is not loaded yet (the desktop then runs on simpledrm; a terminal is enough).
# Output (owned by the sudo user): ~/strix_regtrace.txt.zst, ~/strix_dmesg.txt, ~/strix_linux_regs_after_init.txt
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
if lsmod | grep -q '^amdgpu '; then echo "amdgpu is already loaded: reboot with modprobe.blacklist=amdgpu first"; exit 1; fi
U=${SUDO_USER:-root}; H=$(getent passwd "$U" | cut -d: -f6)
HERE=$(cd "$(dirname "$0")" && pwd)
# When amdgpu takes the display over from simpledrm, the GNOME session running on it crashes and takes the terminal
# (and this script) with it - the first run lost its trace that way. Run detached as a transient system unit instead.
if [ -z "${TRACE_DETACHED:-}" ]; then
    systemctl reset-failed strix-trace 2>/dev/null || true
    systemd-run --unit=strix-trace --collect -E TRACE_DETACHED=1 -E SUDO_USER="$U" /bin/bash "$HERE/$(basename "$0")"
    echo "trace running in the background (unit strix-trace). The desktop may restart to the login screen - that is"
    echo "expected. Wait ~40 s, log in, and check: ls -la $H/strix_regtrace.txt.zst   (progress: journalctl -u strix-trace)"
    exit 0
fi
mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
T=/sys/kernel/tracing
DEV=0000:64:00.0
# The amdgpu trace events exist only once the module is loaded, and loading it binds the GPU at once. So: load the
# module with PCI autoprobe off, enable the events, then bind the GPU by hand.
echo 0 > /sys/bus/pci/drivers_autoprobe
trap 'echo 1 > /sys/bus/pci/drivers_autoprobe' EXIT
modprobe amdgpu
[ -d $T/events/amdgpu ] || { echo "no amdgpu trace events"; exit 1; }
echo 0 > $T/tracing_on
echo 524288 > $T/buffer_size_kb          # per CPU; a full init is a few hundred thousand events
echo 0 > $T/options/overwrite            # if it fills, keep the beginning (PSP .. first IB) and drop the tail
: > $T/trace
echo 1 > $T/events/amdgpu/amdgpu_device_wreg/enable
echo 1 > $T/events/amdgpu/amdgpu_device_rreg/enable
dmesg -C
echo 1 > $T/tracing_on
echo $DEV > /sys/bus/pci/drivers/amdgpu/bind
sleep 20
echo 0 > $T/tracing_on
echo 1 > /sys/bus/pci/drivers_autoprobe
cat $T/trace | zstd -q -f -o "$H/strix_regtrace.txt.zst"   # tracefs reports size 0: stream it
dmesg > "$H/strix_dmesg.txt"
python3 "$HERE/../linux/strix_regdump.py" "$H/strix_linux_regs_after_init.txt" || true
chown "$U" "$H"/strix_regtrace.txt.zst "$H"/strix_dmesg.txt "$H"/strix_linux_regs_after_init.txt 2>/dev/null || true
echo "lines: $(zstd -dc "$H/strix_regtrace.txt.zst" | wc -l)"
echo "done: $H/strix_regtrace.txt.zst $H/strix_dmesg.txt  - reboot normally afterwards"
