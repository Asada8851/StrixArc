SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

P(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl "$@" 2>&1 | tail -1; }
R(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl regs "$@" 2>&1 | grep 0x | tr "\n" " "; echo; }
eval $SUDO_CMD bash -c "nohup shutdown -r +4 >/dev/null 2>&1 &"
P wreg 0xDE0 1 >/dev/null
eval $SUDO_CMD ~/gpu/phxctl/mesboot gart
RING=0x2100000
for o in 0 4 8 12 16 20 24 28; do P vramw $((RING+o)) 0 >/dev/null; done
for a in 0x2101000 0x2101004 0x2101010 0x2101014 0x2102000; do P vramw $a 0 >/dev/null; done
P vramw $((RING+0)) 0xC0033700 >/dev/null; P vramw $((RING+4)) 0x00100500 >/dev/null
P vramw $((RING+8)) 0x42102000 >/dev/null; P vramw $((RING+12)) 0 >/dev/null; P vramw $((RING+16)) 0xFEEDF00D >/dev/null
P wreg 0xA803 0x15000000 >/dev/null
P wreg 0x3041 0x20000709 >/dev/null; P wreg 0x3054 0 >/dev/null; P wreg 0x3055 0 >/dev/null
P wreg 0x3043 0x42101000 >/dev/null; P wreg 0x3044 0 >/dev/null
P wreg 0x30EB 0x42101010 >/dev/null; P wreg 0x30EC 0 >/dev/null
P wreg 0x3041 0x20000709 >/dev/null; P wreg 0x3040 0x421000 >/dev/null; P wreg 0x30B1 0 >/dev/null; P wreg 0x31A0 1 >/dev/null
P wreg 0x30AE 7 >/dev/null; P wreg 0x304B 1 >/dev/null
P wreg 0xA802 0 >/dev/null
P wreg 0xA803 0x01000000 >/dev/null; sleep 0.5; R 0x21a0 0x219e
P wreg 0x3054 5 >/dev/null; sleep 1
P vram 0x2102000 1
R 0x21a0 0x219e 0x21c0 0x3054 0x21a1 0x21a2
eval $SUDO_CMD killall shutdown 2>/dev/null
