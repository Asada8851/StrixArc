#!/bin/bash
# go.sh - every nullarc step on the Mac in one command (no sudo: it asks for the password once):
#   bash /Volumes/<stick>/nullarc/go.sh     (the first run also puts a link on the Desktop: ~/Desktop/nullarc/go.sh)
# The folder is kept current from Linux. go.sh installs whatever in it is newer than what runs (tools, the boot daemon,
# PhxAccel; PhxBringup comes with the stick's EFI) and then either says to reboot or, when everything here is what
# runs, waits for phxboot to finish and runs this round's tests (the file TESTS, one command a line), each of which saves
# its logs on the stick. So: run it; reboot if it says so; run it again.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu
[ "$(id -u)" != 0 ] || { echo "run it without sudo (it asks for the password itself)"; exit 1; }
say() { echo "== $*"; }
[ -e ~/Desktop/nullarc ] || { ln -s "$HERE" ~/Desktop/nullarc && say "link on the Desktop: ~/Desktop/nullarc"; }
sudo -v || exit 1
( while sleep 50; do sudo -n true 2>/dev/null || exit; done ) & KEEP=$!
trap 'kill $KEEP 2>/dev/null' EXIT
WANT=$(cat "$HERE/BUILD" 2>/dev/null || echo 0)
REBOOT=""
need_reboot() { REBOOT="$REBOOT
   - $*"; }

say "round $(cat "$HERE/ROUND" 2>/dev/null || echo ?): PhxBringup $WANT"
# 1. tools and scripts into ~/gpu
bash "$HERE/install.sh" > /tmp/nullarc-install.log 2>&1 || { cat /tmp/nullarc-install.log; say "STOP: install.sh failed"; exit 1; }
say "tools and scripts in ~/gpu"
# 2. PhxBringup: the stick's EFI carries it
HAVE=$(ioreg -rc PhxBringup -d1 | sed -n 's/.*"phx-build" = \([0-9]*\).*/\1/p' | head -1)
if [ "${HAVE:-0}" -lt "$WANT" ]; then need_reboot "PhxBringup ${HAVE:-not loaded} runs, the stick's EFI has $WANT (boot from the stick)"
else say "PhxBringup $HAVE runs"; fi
# 3. PhxAccel (Aux KC): installed copies are re-signed, so the package binary's checksum is kept in $G/.accel-installed
NEWSUM=$(shasum "$HERE/metal/PhxAccel.kext/Contents/MacOS/PhxAccel" | cut -d' ' -f1)
if [ "$(cat $G/.accel-installed 2>/dev/null)" != "$NEWSUM" ]; then
    say "PhxAccel: installing this folder's build"
    sudo bash "$HERE/update_accel.sh" | tail -3
    if [ "${PIPESTATUS[0]}" = 0 ] && echo "$NEWSUM" | sudo tee $G/.accel-installed >/dev/null; then need_reboot "PhxAccel updated (Aux KC)"
    else say "STOP: update_accel.sh failed (above)"; exit 1; fi
else say "PhxAccel is this folder's build"; fi
# 4. the boot daemon and the scripts it runs; also the way back from phxboot's kill switch
changed=""
for f in phxboot.sh full.sh bringup5.sh nullarc_logs.sh; do cmp -s "$HERE/gpu/$f" "$G/$f" || changed="$changed $f"; done
cmp -s "$HERE/gpu/phxctl/mesboot" "$G/phxctl/mesboot" || changed="$changed mesboot"
cmp -s "$HERE/gpu/local.phxboot.plist" /Library/LaunchDaemons/local.phxboot.plist || changed="$changed plist"
OFF=""; [ -e $G/phxboot-off ] && OFF=1
if [ -n "$changed" ] || [ -n "$OFF" ]; then
    say "boot daemon:${changed:+ new$changed}${OFF:+ (kill switch was set: cleared)}; WindowServer on the GPU"
    sudo bash "$HERE/install_boot.sh" | tail -1 && sudo bash "$HERE/install_boot.sh" --windowserver --force | tail -1 ||
        { say "STOP: install_boot.sh failed"; exit 1; }
    need_reboot "the boot daemon changed${OFF:+ (and the GPU was off: kill switch)}"
else say "boot daemon is this folder's"; fi
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || need_reboot "the GPU is not up this boot (/var/log/phxboot.log: $(tail -1 /var/log/phxboot.log))"

# phxboot is through its bring-up and WindowServer watch once its mark is gone, and through the SDMA minute once that
# one is gone too (its hang watcher then runs on for good, so the process itself never ends)
phxboot_settled() { [ ! -e /var/db/phxboot.pending ] && [ ! -e $G/gpucopy.pending ]; }
wait_phxboot() {
    local i
    for i in $(seq 1 60); do phxboot_settled && return 0; [ $i = 1 ] && say "waiting for phxboot to finish its watches (up to 5 min)"; sleep 5; done
    return 1
}
if [ -n "$REBOOT" ]; then
    say "a reboot is needed:$REBOOT"
    pgrep -qf "$G/phxboot.sh" && ! phxboot_settled && wait_phxboot
    printf '== reboot now (from the stick)? [Y/n] '; read -r a
    case "$a" in n|N) say "then reboot later and run go.sh again" ;; *) sudo shutdown -r now ;; esac
    exit 0
fi

# 5. everything here runs: wait for this boot's phxboot (SDMA comes on at its end, ~2.5 min after boot), then the tests
wait_phxboot || say "phxboot has not finished in 5 min: testing anyway"
say "phxboot: $(grep -E '^(SDMA:|frames copied|done)' /var/log/phxboot.log | tail -2 | tr '\n' ' ')"
[ -f "$HERE/NOTE" ] && { echo; cat "$HERE/NOTE"; echo; }
while IFS= read -r t; do
    case "$t" in ''|'#'*) continue ;; esac
    say "test: $t"
    eval "bash \"$HERE/\"$t" < /dev/tty
done < "$HERE/TESTS"
say "done: logs are on the stick (nullarc_logs/)"
