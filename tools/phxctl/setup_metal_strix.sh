#!/bin/bash
# One-time install of the Metal side on macOS 15 (Sequoia) for Strix Point, from Linux-cross-built pieces:
#   - NVMTLDriver.bundle: NullMoth 2.4.1's bundle (Info.plist, resources) with nullarc's NVMTLDriver binary;
#   - /Library/GPUBundles/nvmtl: NullMoth 2.4.1's nvmtl (libnvmtl_translate.dylib, the AIR -> SPIR-V translator) plus
#     our RADV (libvulkan_radeon.dylib, strix1) and radeon_icd.json;
#   - nvmtl-allow.txt: Metal for the test tools only (mtlprobe is on the plugin's built-in list): no app and not
#     WindowServer get the GPU yet;
#   - PhxAccel.kext (built without NM_TAHOE: the macOS 15 accelerator) into the Auxiliary Kernel Collection, after
#     backing the current one up.
# Usage: sudo bash setup_metal.sh <NullMoth 2.4.1 pkgroot> <this package's dir>
# Undo: sudo bash setup_metal.sh --undo
set -u
GB=/Library/GPUBundles; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc; BK=/Users/Shared/nullarc-backup
die() { echo "STOP: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
if [ "${1:-}" = --undo ]; then
    rm -rf "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt" /Library/Extensions/PhxAccel.kext
    last=$(ls -t "$BK"/AuxiliaryKernelExtensions.kc.* 2>/dev/null | head -1)
    [ -n "$last" ] && cp -p "$last" "$KC" && echo "aux KC restored from $last"
    echo "Metal pieces removed; reboot"; exit 0
fi
NM=${1:?usage: sudo bash setup_metal.sh <NullMoth 2.4.1 pkgroot> <package dir>}; PKG=${2:?package dir}
[ "$(sw_vers -productVersion | cut -d. -f1)" = 15 ] || die "this build is for macOS 15 (the accelerator has no NM_TAHOE)"
[ -f "$NM/Library/GPUBundles/NVMTLDriver.bundle/Contents/Info.plist" ] || die "$NM is not NullMoth's pkgroot (no Library/GPUBundles/NVMTLDriver.bundle)"
# 2.4.1 ships the translator inside the bundle (Contents/MacOS, next to the plugin, where the plugin looks first);
# older pkgroots had it in Library/GPUBundles/nvmtl. Take whichever exists.
XL=$(find "$NM" -name libnvmtl_translate.dylib -type f 2>/dev/null | head -1)
[ -n "$XL" ] || die "no libnvmtl_translate.dylib anywhere in $NM"
echo "translator: ${XL#$NM/}"
for f in metal/NVMTLDriver metal/PhxAccel.kext/Contents/MacOS/PhxAccel gpu/nvmtl/libvulkan_radeon.dylib; do
    [ -e "$PKG/$f" ] || die "missing $PKG/$f"
done
csrutil status | grep -q 'disabled\|Unknown\|Custom' || echo "warning: csrutil reports SIP fully enabled; the aux KC rebuild will be refused"

mkdir -p "$BK"; TS=$(date +%Y%m%d-%H%M%S)
[ -f "$KC" ] && cp -p "$KC" "$BK/AuxiliaryKernelExtensions.kc.$TS" && echo "aux KC backed up: $BK/AuxiliaryKernelExtensions.kc.$TS"

echo "== Metal plugin"
rm -rf "$GB/NVMTLDriver.bundle" "$GB/nvmtl"
mkdir -p "$GB"
ditto "$NM/Library/GPUBundles/NVMTLDriver.bundle" "$GB/NVMTLDriver.bundle" || die "copy bundle"
cp "$PKG/metal/NVMTLDriver" "$GB/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver" || die "copy plugin binary"
if [ -d "$NM/Library/GPUBundles/nvmtl" ]; then ditto "$NM/Library/GPUBundles/nvmtl" "$GB/nvmtl" || die "copy nvmtl"; fi
mkdir -p "$GB/nvmtl"
[ -f "$GB/nvmtl/libnvmtl_translate.dylib" ] || cp "$XL" "$GB/nvmtl/libnvmtl_translate.dylib" || die "copy translator"
codesign --force --sign - "$GB/nvmtl/libnvmtl_translate.dylib" >/dev/null 2>&1
cp "$PKG/gpu/nvmtl/libvulkan_radeon.dylib" "$GB/nvmtl/libvulkan_radeon.dylib"
cat > "$GB/nvmtl/radeon_icd.json" <<'J'
{ "file_format_version": "1.0.1", "ICD": { "library_path": "/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib", "library_arch": "64", "api_version": "1.4.363" } }
J
printf '# nullarc Strix Point bring-up: Metal for the test tools only (mtlprobe is built in)\n' > "$GB/nvmtl-allow.txt"
xattr -dr com.apple.quarantine "$GB/NVMTLDriver.bundle" "$GB/nvmtl" 2>/dev/null
codesign --force --sign - "$GB/nvmtl/libvulkan_radeon.dylib" >/dev/null 2>&1
codesign --force --sign - "$GB/NVMTLDriver.bundle" 2>&1 | tail -1
chown -R root:wheel "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt"
chmod -R go-w,a+rX "$GB/NVMTLDriver.bundle" "$GB/nvmtl" && chmod 644 "$GB/nvmtl-allow.txt"
mkdir -p "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache" && chown -R _windowserver:wheel "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"
chmod 0777 "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"   # the test tools run as you, WindowServer as _windowserver
codesign --verify --strict "$GB/NVMTLDriver.bundle" && echo "plugin signature ok: $(ls "$GB/nvmtl" | tr '\n' ' ')"

echo "== PhxAccel into the aux KC"
rm -rf /Library/Extensions/PhxAccel.kext
ditto "$PKG/metal/PhxAccel.kext" /Library/Extensions/PhxAccel.kext || die "copy PhxAccel"
xattr -dr com.apple.quarantine /Library/Extensions/PhxAccel.kext 2>/dev/null
codesign --force --sign - /Library/Extensions/PhxAccel.kext >/dev/null 2>&1
chown -R root:wheel /Library/Extensions/PhxAccel.kext && chmod -R 755 /Library/Extensions/PhxAccel.kext
NEW=$KC.nullarc-new; rm -f "$NEW"
kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions -A "$NEW" -z > /tmp/nullarc-kmutil.log 2>&1
echo "kmutil rc=$?"
if ! kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null | grep -q com.luuumi.PhxAccel; then
    echo "PhxAccel did not make it into the new KC; kmutil said:"; grep -i -E 'error|fail|unresolved|missing|symbol' /tmp/nullarc-kmutil.log | head -20
    rm -f "$NEW"; die "aux KC left as it was"
fi
mv -f "$NEW" "$KC" && echo "aux KC published with PhxAccel"
echo "== done. Allow the extension if System Settings -> Privacy & Security asks, then reboot, sudo bash install_boot.sh, reboot again and run: bash ~/gpu/try15.sh"
