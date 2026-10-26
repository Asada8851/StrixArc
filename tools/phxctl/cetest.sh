SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

P(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl "$@" 2>&1 | tail -1; }
R(){ eval $SUDO_CMD ~/gpu/phxctl/phxctl regs "$@" 2>&1 | grep 0x | tr "\n" " "; echo; }
eval $SUDO_CMD bash -c "nohup shutdown -r +4 >/dev/null 2>&1 &"
echo "before:"; R 0x21a0 0x219e 0xa803 0xa802
P wreg 0xA802 0 >/dev/null
P wreg 0xA803 0x01000000 >/dev/null
sleep 1; echo "after ME_CNTL=0x01000000:"; R 0x21a0 0x219e 0x21a1 0x21a2 0xa803
eval $SUDO_CMD killall shutdown 2>/dev/null
