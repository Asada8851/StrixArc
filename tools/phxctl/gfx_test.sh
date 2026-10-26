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
eval $SUDO_CMD bash -c 'nohup shutdown -r +3 >/dev/null 2>&1 &'
echo "== precheck =="; P regs 0x263e 0x21a0 | grep 0x
RING=0x2100000; RPTRWB=0x2101000; WPTRSH=0x2101010; TGT=0x2102000
for o in 0 4 8 12 16 20 24 28; do P vramw $((RING+o)) 0 >/dev/null; done
for a in $RPTRWB $((RPTRWB+4)) $WPTRSH $((WPTRSH+4)) $TGT; do P vramw $a 0 >/dev/null; done
P vramw $((RING+0))  0xC0033700 >/dev/null
P vramw $((RING+4))  0x00100500 >/dev/null
P vramw $((RING+8))  0x02102000 >/dev/null
P vramw $((RING+12)) 0x00000080 >/dev/null
P vramw $((RING+16)) 0xFEEDF00D >/dev/null
P vram $RING 5 | grep vram+
echo "== program CP_RB0 =="
P wreg 0x21C1 0 >/dev/null; P wreg 0x3051 0 >/dev/null
P wreg 0x3041 0x20000709 | tail -1
P wreg 0x3054 0 >/dev/null; P wreg 0x3055 0 >/dev/null
P wreg 0x3043 0x02101000 >/dev/null; P wreg 0x3044 0x80 >/dev/null
P wreg 0x30EB 0x02101010 >/dev/null; P wreg 0x30EC 0x80 >/dev/null
P wreg 0x3041 0x20000709 >/dev/null
P wreg 0x3040 0x80021000 >/dev/null; P wreg 0x30B1 0 >/dev/null
P wreg 0x31A0 1 | tail -1
P wreg 0x30AE 7 >/dev/null; P wreg 0x304B 1 >/dev/null
echo "== unhalt ME/PFP/CE =="
P wreg 0xA803 0 | tail -1
sleep 1; P regs 0x21a0 0x2004 | grep 0x
echo "== kick wptr=5 =="
P wreg 0x3054 5 | tail -1
sleep 1
echo "== RESULT (expect feedf00d) =="
P vram $TGT 2 | grep vram+
P regs 0x21c0 0x3054 0x21a0 0xa803 | grep 0x
eval $SUDO_CMD killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
