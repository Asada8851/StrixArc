SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

set -u

SUDO_CMD=\"sudo\"
if [ -n \"$SUDO_PASS\" ]; then
    SUDO_CMD=\"echo '$SUDO_PASS' | sudo -S -p ''\"
fi
P(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl "$@"; }
echo "== watchdog: reboot in 3 min unless cancelled =="
eval $SUDO_CMD bash -c 'nohup shutdown -r +3 >/dev/null 2>&1 &'
P psp destroy >/dev/null 2>&1
P psp create 0x1100000 >/dev/null
P stage ~/gpu/fw/psp_13_0_4_toc.bin 256 2304 0x0 >/dev/null
P raw 0x20 0x04000000 0x80 0x900 0 0 0 >/dev/null
P raw 0x5 0x08000000 0x80 0x4000000 0x2 0x0C000000 0x6A >/dev/null
echo "== load firmware =="
L(){ P loadfw "$@" 2>&1 | grep loadfw >> ~/gpu/fwaddr.log; }
: > ~/gpu/fwaddr.log
L ~/gpu/fw/gc_11_0_1_pfp.bin 256 263168 2 0x000000
L ~/gpu/fw/gc_11_0_1_me.bin  256 263168 1 0x100000
L ~/gpu/fw/gc_11_0_1_mec.bin 256 267008 4 0x200000
L ~/gpu/fw/gc_11_0_1_rlc.bin 256 25088  8 0x300000
L ~/gpu/fw/gc_11_0_1_rlc.bin 25344 2560 20 0x700000
L ~/gpu/fw/gc_11_0_1_rlc.bin 27904 21104 21 0x720000
L ~/gpu/fw/gc_11_0_1_rlc.bin 49008 66048 26 0x800000
L ~/gpu/fw/gc_11_0_1_rlc.bin 115056 33280 48 0x840000
L ~/gpu/fw/gc_11_0_1_rlc.bin 148336 8704 25 0x880000
L ~/gpu/fw/sdma_6_0_1.bin    256 17408 71 0x380000
L ~/gpu/fw/sdma_6_0_1.bin    17664 16896 72 0x3C0000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 256 135648 33 0x400000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 135904 131072 34 0x440000
L ~/gpu/fw/gc_11_0_1_mes1.bin 256 104272 81 0x480000
L ~/gpu/fw/gc_11_0_1_mes1.bin 104528 131072 82 0x4C0000
L ~/gpu/fw/gc_11_0_1_imu.bin 256 66048 68 0x500000
L ~/gpu/fw/gc_11_0_1_imu.bin 66304 66048 69 0x600000
echo "-- GFX regs BEFORE EnableGfxImu: GB_ADDR_CONFIG(0x263e) GRBM(0x2004) GFX_IMU_GFX_RESET_CTRL(?)"
P regs 0x263e 0x2004 0x21a0
echo "== SMU EnableGfxImu (0x16) =="
P smu 0x16 0
sleep 1
echo "-- GFX regs AFTER EnableGfxImu:"
P regs 0x263e 0x2004 0x21a0 0xee7e
echo "== AUTOLOAD_RLC =="
P autoload
sleep 1
echo "-- after autoload: BOOTLOAD(0xee7e) CP_STAT(0x21a0) GRBM(0x2004) GB_ADDR(0x263e)"
P regs 0xee7e 0x21a0 0x2004 0x263e
echo "== gc summary =="
P gc 2>/dev/null | head -8

set -u
P(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl "$@"; }
echo "== precheck: GPU alive? SDMA0_STATUS GB_ADDR =="; P regs 0x1285 0x263e | grep 0x
RING=0x2000000; RPTRWB=0x2001000; WPTRSH=0x2001010; TGT=0x2002000
echo "== zero ring/rptr/wptr-shadow/target =="
for o in 0 4 8 12 16 20 24 28; do P vramw $((RING+o)) 0 >/dev/null; done
for a in $RPTRWB $((RPTRWB+4)) $WPTRSH $((WPTRSH+4)) $TGT; do P vramw $a 0 >/dev/null; done
echo "== write SDMA WRITE_LINEAR packet into ring =="
P vramw $((RING+0))  0x00000002 >/dev/null
P vramw $((RING+4))  0x02002000 >/dev/null
P vramw $((RING+8))  0x00000080 >/dev/null
P vramw $((RING+12)) 0x00000000 >/dev/null
P vramw $((RING+16)) 0xDEADBEEF >/dev/null
P vram $RING 5 | grep vram+
echo "== program SDMA0 queue0 =="
P wreg 0x12E0 0x00040800 | tail -1             # RB disabled, base config
P wreg 0x12E3 0 >/dev/null; P wreg 0x12E4 0 >/dev/null     # RPTR/RPTR_HI
P wreg 0x12E5 0 >/dev/null; P wreg 0x12E6 0 >/dev/null     # WPTR/WPTR_HI
P wreg 0x1313 0x02001010 >/dev/null; P wreg 0x1312 0x80 >/dev/null   # wptr poll addr lo/hi
P wreg 0x12E8 0x80 >/dev/null; P wreg 0x12E9 0x02001000 >/dev/null  # rptr wb addr hi/lo
# RB_CNTL: RB_SIZE=10 (<<1)=0x14, RPTR_WRITEBACK(12)=0x1000, F32_WPTR_POLL(11)=0x800, RB_PRIV(23)=0x800000
P wreg 0x12E0 0x00841814 | tail -1
P wreg 0x12E1 0x80020000 >/dev/null; P wreg 0x12E2 0 >/dev/null       # RB_BASE = mc>>8, HI = mc>>40
P wreg 0x1315 1 >/dev/null                                              # MINOR_PTR_UPDATE
P wreg 0x12E5 0 >/dev/null; P wreg 0x12E6 0 >/dev/null
P wreg 0x1315 0 >/dev/null
echo "== unhalt F32, enable RB + IB =="
P wreg 0xF89A 0x08084000 | tail -1
P wreg 0x12E0 0x00841815 | tail -1
P wreg 0x12EA 0x00000101 | tail -1
echo "== kick: shadow wptr then register wptr = 0x14 bytes =="
P vramw $WPTRSH 0x14 >/dev/null
P wreg 0x12E5 0x14 | tail -1
sleep 1
echo "== RESULT: target (expect deadbeef), rptr wb, RPTR reg, WPTR reg, SDMA status =="
P vram $TGT 2 | grep vram+
P vram $RPTRWB 2 | grep vram+
P regs 0x12E3 0x12E5 0x1285 0x12E0 | grep 0x
uptime
eval $SUDO_CMD killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
