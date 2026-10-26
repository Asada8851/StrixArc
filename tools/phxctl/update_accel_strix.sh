#!/bin/bash
# Replace PhxAccel.kext (the Metal accelerator + PhxAMDFB) in the Auxiliary Kernel Collection with this package's build,
# as setup_metal.sh did the first time, keeping the current kext and KC for the way back. Takes effect at the next boot.
#   sudo bash update_accel.sh          install metal/PhxAccel.kext from this package
#   sudo bash update_accel.sh --undo   put back the kext and the aux KC from before the last update
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc; BK=/Users/Shared/nullarc-backup; LE=/Library/Extensions/PhxAccel.kext
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "STOP: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
if [ "${1:-}" = --undo ]; then
    last=$(ls -td "$BK"/accel-update.* 2>/dev/null | head -1)
    [ -n "$last" ] || die "no earlier update in $BK"
    rm -rf "$LE"; ditto "$last/PhxAccel.kext" "$LE" && cp -p "$last/AuxiliaryKernelExtensions.kc" "$KC" || die "restore from $last failed"
    chown -R root:wheel "$LE"
    echo "restored from $last; reboot"; exit 0
fi
[ "$(sw_vers -productVersion | cut -d. -f1)" = 15 ] || die "this PhxAccel build is for macOS 15"
NEWK="$HERE/metal/PhxAccel.kext"
[ -f "$NEWK/Contents/MacOS/PhxAccel" ] || die "no $NEWK in this package"
[ -d "$LE" ] || die "no $LE: run setup_metal.sh first"
TS=$(date +%Y%m%d-%H%M%S); SAVE="$BK/accel-update.$TS"
mkdir -p "$SAVE" && ditto "$LE" "$SAVE/PhxAccel.kext" && cp -p "$KC" "$SAVE/AuxiliaryKernelExtensions.kc" || die "backup to $SAVE failed"
echo "saved the current PhxAccel and aux KC in $SAVE"
rm -rf "$LE"; ditto "$NEWK" "$LE" || die "copy failed (restore: sudo bash update_accel.sh --undo)"
xattr -dr com.apple.quarantine "$LE" 2>/dev/null
codesign --force --sign - "$LE" >/dev/null 2>&1
chown -R root:wheel "$LE" && chmod -R 755 "$LE"
NEW=$KC.nullarc-new; rm -f "$NEW"
kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions -A "$NEW" -z > /tmp/nullarc-kmutil.log 2>&1
echo "kmutil rc=$?"
if ! kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null | grep -q com.luuumi.PhxAccel; then
    echo "PhxAccel did not make it into the new KC; kmutil said:"; grep -i -E 'error|fail|unresolved|missing|symbol' /tmp/nullarc-kmutil.log | head -20
    rm -f "$NEW"; rm -rf "$LE"; ditto "$SAVE/PhxAccel.kext" "$LE"; chown -R root:wheel "$LE"
    die "the old PhxAccel and aux KC are left as they were"
fi
mv -f "$NEW" "$KC" && echo "aux KC published with the new PhxAccel. Reboot (allow the extension if Privacy & Security asks), then: bash ~/gpu/fps_check.sh"
