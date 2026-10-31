#!/bin/bash
# Replace the Metal plugin's binary (NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver; NullMoth's bundle, nullarc's binary)
# with this package's build, keeping the current one for the way back. WindowServer loads it once: takes effect at the
# next boot (other apps: at their next start).
#   sudo bash update_plugin.sh          install metal/NVMTLDriver from this package
#   sudo bash update_plugin.sh --undo   put back the binary from before the last update
set -u
B=/Library/GPUBundles/NVMTLDriver.bundle; BIN=$B/Contents/MacOS/NVMTLDriver; BK=/Users/Shared/nullarc-backup
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "STOP: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
sign() { xattr -dr com.apple.quarantine "$B" 2>/dev/null; codesign --force --sign - "$B" >/dev/null 2>&1
         chown -R root:wheel "$B"; chmod -R go-w,a+rX "$B"; codesign --verify --strict "$B" || die "the bundle's signature does not verify"; }
if [ "${1:-}" = --undo ]; then
    last=$(ls -td "$BK"/plugin-update.* 2>/dev/null | head -1)
    [ -n "$last" ] || die "no earlier update in $BK"
    cp "$last/NVMTLDriver" "$BIN" || die "restore from $last failed"
    sign; echo "restored from $last; reboot"; exit 0
fi
[ -f "$HERE/metal/NVMTLDriver" ] || die "no metal/NVMTLDriver in this package"
[ -f "$BIN" ] || die "no $BIN: run setup_metal.sh first"
TS=$(date +%Y%m%d-%H%M%S); SAVE="$BK/plugin-update.$TS"
mkdir -p "$SAVE" && cp -p "$BIN" "$SAVE/NVMTLDriver" || die "backup to $SAVE failed"
echo "saved the current plugin binary in $SAVE"
cp "$HERE/metal/NVMTLDriver" "$BIN" || die "copy failed (restore: sudo bash update_plugin.sh --undo)"
sign; echo "plugin binary installed ($(shasum "$HERE/metal/NVMTLDriver" | cut -c1-12)); reboot"
