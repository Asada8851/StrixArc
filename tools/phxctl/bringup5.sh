#!/bin/bash
# GFX power-up in the Linux driver's order: PSP ring, TOC and TMR, the firmware of every engine through LOAD_IP_FW, RLC
# autoload, then the SMU messages that switch GFX on (EnableGfxImu with 1). Firmware from ~/gpu/fw. Run as root
# (phxboot) or as a user with sudo. A reboot is armed for 3 minutes in case the PSP hangs and cancelled at the end.
#
# Phoenix (the default): the offsets and sizes of the gc_11_0_1/psp_13_0_4 files the reference machine has, as before.
# Strix Point (PhxBringup's phx-chip property, or CHIP=strix-point): the plan fwplan reads from the gc_11_5_0 /
# sdma_6_1_0 / psp_14_0_0 headers, into the same fw_buf slots, after LOAD_TOC and SETUP_TMR. MP0 14.0.0 has
# boot_time_tmr = false in Linux amdgpu_psp.c (v6.10 .. master), so the driver owns the TMR: it takes the size from the
# LOAD_TOC response and SETUP_TMRs a VRAM buffer of that size (on the 880M: "reserve 0x8900000 from 0x80e0000000 for PSP
# TMR"). Without it the PSP returned firmware addresses no TMR backs, refused IMU_I (0xFFFF000B), and the gfx PFP
# faulted (CPG, VMID 0, a different address every boot) on the first INDIRECT_BUFFER, a code path it fetches from memory.
# STRIX_TMR=0 restores the old behaviour (no SETUP_TMR) for comparison.
set -u
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
P(){ $SUDO ~/gpu/phxctl/phxctl "$@"; }
die(){ echo "STOP: $*"; $SUDO killall shutdown 2>/dev/null; exit 1; }
st(){ sed -n 's/.*status \(0x[0-9a-fA-F]*\).*/\1/p'; }
kprop(){ ioreg -rc PhxBringup -d1 | sed -n "s/.*\"$1\" = \([0-9][0-9a-fA-Fx]*\).*/\1/p" | head -1; }
CHIP=${CHIP:-$(ioreg -l | sed -n 's/.*"phx-chip" = "\(.*\)".*/\1/p' | head -1)}
CHIP=${CHIP:-phoenix}
echo "== chip: $CHIP =="
case $CHIP in phoenix|strix-point) ;; *) echo "unknown chip $CHIP"; exit 1 ;; esac
# psp create offset; the kext puts the GPCOM cmd buffer at ring + 16 KiB. 17 MiB lies inside a 3072x1920 firmware frame
# at offset 0 (22.5 MiB): on Strix with PhxBringup 37+ (which accepts it) the ring moves to 48 MiB, above the frame and
# the queues (0x2400000..0x2801000), below fw_buf (64 MiB).
PSP_RING=0x1100000
[ "$CHIP" = strix-point ] && [ "$(kprop phx-build)" -ge 37 ] 2>/dev/null && PSP_RING=0x3000000
echo "== PSP ring at $PSP_RING =="
PSP_RESP=$((PSP_RING + 0x4000 + 864))    # psp_gfx_resp inside it: status +0, fw_addr_lo/hi +8/+12, tmr_size +16
# NMXAccel runs only after this boot's bring-up and claim (phxboot, or a test script): tearing the PSP ring down under it
# hangs its clients. One bring-up per boot.
if ioreg -rc NMXAccel -d0 2>/dev/null | grep -q NMXAccel && [ "${BRINGUP_FORCE:-0}" != 1 ]; then
    echo "STOP: the GPU is already up this boot (NMXAccel runs: phxboot or an earlier run did the bring-up). Reboot first"
    exit 1
fi
echo "== watchdog: reboot in 3 min unless cancelled =="
$SUDO bash -c 'nohup shutdown -r +3 >/dev/null 2>&1 &'
P psp destroy >/dev/null 2>&1
P psp create $PSP_RING >/dev/null
L(){ P loadfw "$@" 2>&1 | grep loadfw >> ~/gpu/fwaddr.log; }
: > ~/gpu/fwaddr.log
if [ "$CHIP" = strix-point ]; then
    P stage ~/gpu/fw/psp_14_0_0_toc.bin 256 2304 0x0 >/dev/null     # same TOC layout as psp_13_0_4 (fwplan-checked)
    toc=$(P raw 0x20 0x04000000 0x80 0x900 0 0 0)
    [ "$(echo "$toc" | st)" = 0x00000000 ] || die "LOAD_TOC: $toc"
    # tmr_size from the LOAD_TOC response (read before the next command clears the buffer), as psp_tmr_init does
    TMR_SIZE=$(P vram $PSP_RESP 8 | awk '/vram\+/{print "0x"$6; exit}')
    echo "LOAD_TOC ok, PSP asks for a TMR of ${TMR_SIZE:-?} bytes"
    if [ "${STRIX_TMR:-1}" != 0 ]; then
        FBMC=$(kprop phx-fb-mc-base); FBSZ=$(kprop phx-fb-size); FBPA=$(kprop phx-fb-phys)
        [ -n "$FBMC" ] && [ -n "$FBSZ" ] && [ -n "$FBPA" ] && [ $((FBSZ)) -ge $((0x40000000)) ] ||
            die "no FB layout from PhxBringup (mc=$FBMC size=$FBSZ phys=$FBPA)"
        if [ -z "$TMR_SIZE" ] || [ $((TMR_SIZE)) -eq 0 ] || [ $((TMR_SIZE)) -gt $((0x10000000)) ]; then
            echo "note: implausible tmr_size '$TMR_SIZE', using 0x8900000 (what Linux reserves on this 880M)"
            TMR_SIZE=0x8900000
        fi
        TMR_SIZE=$(( (TMR_SIZE + 0xFFFFF) & ~0xFFFFF ))       # PSP_TMR_ALIGNMENT, 1 MiB
        # Where Linux's allocator put it on this machine: 512 MiB below the top of VRAM (0x80e0000000 of 4 GiB).
        # Far above the 256 MiB BAR window, the kext's PSP/fw_buf/ring regions and the GOP framebuffer.
        TMR_OFF=$(( FBSZ - 0x20000000 ))
        [ $((TMR_OFF + TMR_SIZE)) -le $((FBSZ)) ] && [ $TMR_OFF -ge $((0x10000000)) ] || die "TMR does not fit VRAM"
        TMR_MC=$((FBMC + TMR_OFF)); TMR_PA=$((FBPA + TMR_OFF))
        printf 'TMR: vram+0x%x size 0x%x  mc 0x%x  system pa 0x%x\n' $TMR_OFF $TMR_SIZE $TMR_MC $TMR_PA
        # psp_prep_tmr_cmd_buf: buf_phy_addr = MC, buf_size, virt_phy_addr (bit 1), system_phy_addr
        tmr=$(P raw 0x5 $(printf '0x%x 0x%x 0x%x 0x2 0x%x 0x%x' $((TMR_MC & 0xFFFFFFFF)) $((TMR_MC >> 32)) \
              $TMR_SIZE $((TMR_PA & 0xFFFFFFFF)) $((TMR_PA >> 32))))
        echo "SETUP_TMR: $tmr"
        [ "$(echo "$tmr" | st)" = 0x00000000 ] || die "SETUP_TMR refused (a second bring-up in the same boot? reboot first)"
        printf '%s %s %s %s\n' $TMR_MC $TMR_SIZE $TMR_OFF $TMR_PA > ~/gpu/tmr.txt
    else
        echo "STRIX_TMR=0: no SETUP_TMR (the try1..try7 behaviour)"; : > ~/gpu/tmr.txt
    fi
    echo "== load firmware (fwplan) =="
    # fw_buf slot per PSP firmware type, as Phoenix uses them, with each slot's size (up to the next one)
    slot() { case $1 in
        2) echo 0x000000 0x100000;; 1) echo 0x100000 0x100000;; 4) echo 0x200000 0x100000;; 8) echo 0x300000 0x80000;;
        71) echo 0x380000 0x40000;; 72) echo 0x3C0000 0x40000;; 33) echo 0x400000 0x40000;; 34) echo 0x440000 0x40000;;
        81) echo 0x480000 0x40000;; 82) echo 0x4C0000 0x40000;; 68) echo 0x500000 0x100000;; 69) echo 0x600000 0x100000;;
        20) echo 0x700000 0x20000;; 21) echo 0x720000 0xE0000;; 26) echo 0x800000 0x40000;; 48) echo 0x840000 0x40000;;
        25) echo 0x880000 0x80000;; *) return 1;; esac; }
    plan=$(~/gpu/phxctl/fwplan ~/gpu/fw gc_11_5_0 sdma_6_1_0.bin) || { echo "fwplan failed"; exit 1; }
    while read -r file off size type; do
        case $file in \#*|"") continue;; esac
        set -- $(slot "$type") || { echo "no fw_buf slot for type $type ($file)"; exit 1; }
        [ $((size)) -le $(($2)) ] || { echo "$file type $type: $size bytes do not fit slot $1"; exit 1; }
        L ~/gpu/fw/$file "$off" "$size" "$type" "$1"
    done <<< "$plan"
    sed 's/^loadfw [^ ]*\/gpu\/fw\//  /' ~/gpu/fwaddr.log
    bad=$(grep -vc 'status 0x00000000' ~/gpu/fwaddr.log)
    [ "$(wc -l < ~/gpu/fwaddr.log)" -ge 17 ] || die "only $(wc -l < ~/gpu/fwaddr.log) of 17 LOAD_IP_FW done"
    [ "$bad" = 0 ] || echo "WARNING: $bad LOAD_IP_FW command(s) failed (see status above)"
else
P stage ~/gpu/fw/psp_13_0_4_toc.bin 256 2304 0x0 >/dev/null
P raw 0x20 0x04000000 0x80 0x900 0 0 0 >/dev/null
P raw 0x5 0x08000000 0x80 0x4000000 0x2 0x0C000000 0x6A >/dev/null
echo "== load firmware =="
L ~/gpu/fw/gc_11_0_1_pfp.bin 256 263168 2 0x000000
L ~/gpu/fw/gc_11_0_1_me.bin  256 263168 1 0x100000
L ~/gpu/fw/gc_11_0_1_mec.bin 256 267008 4 0x200000
L ~/gpu/fw/sdma_6_0_1.bin    256 17408 71 0x380000
L ~/gpu/fw/sdma_6_0_1.bin    17664 16896 72 0x3C0000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 256 135648 33 0x400000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 135904 131072 34 0x440000
L ~/gpu/fw/gc_11_0_1_mes1.bin 256 104272 81 0x480000
L ~/gpu/fw/gc_11_0_1_mes1.bin 104528 131072 82 0x4C0000
L ~/gpu/fw/gc_11_0_1_imu.bin 256 66048 68 0x500000
L ~/gpu/fw/gc_11_0_1_imu.bin 66304 66048 69 0x600000
L ~/gpu/fw/gc_11_0_1_rlc.bin 25344 2560 20 0x700000
L ~/gpu/fw/gc_11_0_1_rlc.bin 27904 21104 21 0x720000
L ~/gpu/fw/gc_11_0_1_rlc.bin 49008 66048 26 0x800000
L ~/gpu/fw/gc_11_0_1_rlc.bin 115056 33280 48 0x840000
L ~/gpu/fw/gc_11_0_1_rlc.bin 148336 8704 25 0x880000
L ~/gpu/fw/gc_11_0_1_rlc.bin 256 25088  8 0x300000
fi
echo "== AUTOLOAD_RLC =="
P autoload
echo "== SMU 0x3, 0x2, EnableGfxImu(param=1) (linux order) =="
P smu 0x3 0 | tail -1
P smu 0x2 0 | tail -1
P smu 0x16 1 | tail -1
sleep 2
P regs 0xee7e 0x21a0 0x2004 0x263e
if [ "$CHIP" = strix-point ]; then
    # Where the F32 CP engines fetch their code from (CP_*_IC_BASE_LO/HI, GC 11.5 segment 1). The PSP programs the
    # TMR's system physical address there (try8: 0x867e52000 = TMR pa 0x860000000 + the fw_addr offset 0x7e52000);
    # without a TMR it was the bare offset, i.e. physical memory macOS owns.
    rv(){ P regs $1 | awk '{print $NF}'; }
    read -r TMR_MC TMR_SIZE _ TMR_PA < ~/gpu/tmr.txt 2>/dev/null || TMR_MC=""
    for e in PFP:0xf840 ME:0xf844 MEC:0xf84c; do
        lo=$(rv ${e#*:}); hi=$(rv $(printf '0x%x' $((${e#*:} + 1))))
        a=$(( (hi << 32) | (lo & 0xFFFFF000) ))
        w="(no TMR set up: system memory the PSP was never given)"
        if [ -n "$TMR_MC" ]; then
            w="OUTSIDE the TMR"
            [ $a -ge $((TMR_MC)) ] && [ $a -lt $((TMR_MC + TMR_SIZE)) ] && w="inside the TMR (MC)"
            [ -n "${TMR_PA:-}" ] && [ $a -ge $((TMR_PA)) ] && [ $a -lt $((TMR_PA + TMR_SIZE)) ] && w="inside the TMR (system pa)"
        fi
        printf '%s IC base 0x%x %s\n' ${e%%:*} $a "$w"
    done
fi
$SUDO killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
