#!/bin/bash
# Replace the RADV the Metal plugin (and so WindowServer) runs on, /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib,
# with this package's (gpu/nvmtl/libvulkan_radeon.dylib), keeping the current one for the way back. Takes effect at the
# next boot. The new one is on trial ($G/radv.pending): phxboot keeps it once WindowServer got through its 90 s watch
# on it, and puts the previous one back by itself after a boot that did not (a hang, WindowServer crashing).
#   sudo bash update_radv.sh          install this package's RADV
#   sudo bash update_radv.sh --undo   put back the RADV from before the last update (reboot after)
set -u
G=/Users/Shared/gpu; RADV=/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib; BK=/Users/Shared/nullarc-backup
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "STOP: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
fix() { xattr -d com.apple.quarantine "$RADV" 2>/dev/null; codesign --force --sign - "$RADV" >/dev/null 2>&1
        chown root:wheel "$RADV"; chmod 755 "$RADV"; codesign --verify "$RADV" || die "the RADV's signature does not verify"; }
if [ "${1:-}" = --undo ]; then
    last=$(ls -td "$BK"/radv-update.* 2>/dev/null | head -1)
    [ -n "$last" ] || die "no earlier update in $BK"
    cp "$last/libvulkan_radeon.dylib" "$RADV" || die "restore from $last failed"
    fix; rm -f $G/radv.pending; sync; echo "restored from $last; reboot"; exit 0
fi
NEW=$HERE/nvmtl/libvulkan_radeon.dylib; [ -f "$NEW" ] || NEW=$HERE/gpu/nvmtl/libvulkan_radeon.dylib
[ -f "$NEW" ] || die "no gpu/nvmtl/libvulkan_radeon.dylib in this package"
[ -f "$RADV" ] || die "no $RADV: run setup_metal.sh first"
TS=$(date +%Y%m%d-%H%M%S); SAVE="$BK/radv-update.$TS"
mkdir -p "$SAVE" && cp -p "$RADV" "$SAVE/libvulkan_radeon.dylib" || die "backup to $SAVE failed"
echo "saved the current RADV in $SAVE"
cp "$NEW" "$RADV" || { cp "$SAVE/libvulkan_radeon.dylib" "$RADV"; die "copy failed: the previous RADV is back"; }
fix
mkdir -p $G && printf 'new\n%s\n' "$SAVE/libvulkan_radeon.dylib" > $G/radv.pending; sync
echo "RADV installed ($(shasum "$NEW" | cut -c1-12)), on trial until WindowServer's watch passes at the next boot; reboot"
