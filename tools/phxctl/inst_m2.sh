#!/bin/bash
# M2 test install: PhxAccel (with PhxFB) into the Aux KC, the patched NVMTLDriver into the bundle, WindowServer allow-list.
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc; BK=/Users/Shared/gpu/backup; TS=$(date +%Y%m%d-%H%M%S)
rm -rf /Library/Extensions/PhxAccel.kext
cp -R /Users/Shared/gpu/phxaccel/build/PhxAccel.kext /Library/Extensions/ && chown -R root:wheel /Library/Extensions/PhxAccel.kext && chmod -R 755 /Library/Extensions/PhxAccel.kext
NEW=$KC.phx-new; rm -f $NEW
kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions -A $NEW -z > /tmp/phx-kmutil.log 2>&1
echo "kmutil rc=$?"
INS=$(kmutil inspect -a x86_64 -A $NEW 2>/dev/null)
for b in com.luuumi.PhxAccel com.highpoint-tech.kext.HighPointIOP com.highpoint-tech.kext.HighPointRR com.apple.driver.AppleMobileDevice; do
  echo "$INS" | grep -q "$b" || { echo "  MISSING: $b -- not publishing"; tail -5 /tmp/phx-kmutil.log; exit 1; }
done
mv -f $NEW $KC && echo "aux KC published"
B=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS
[ -f $BK/NVMTLDriver.nullmoth-2.4.1 ] || cp -p $B/NVMTLDriver $BK/NVMTLDriver.nullmoth-2.4.1
cp /Users/Shared/gpu/nmplugin/NVMTLDriver $B/NVMTLDriver && chown root:wheel $B/NVMTLDriver && chmod 755 $B/NVMTLDriver
codesign --force --sign - /Library/GPUBundles/NVMTLDriver.bundle 2>&1 | tail -1
codesign --verify --strict /Library/GPUBundles/NVMTLDriver.bundle && echo "plugin replaced (patched build), signature ok"
# every app keeps Metal (phxboot writes the same list at boot): an apps-only list here left new apps without a device
printf '# PhxAccel M2: Metal for every app; WindowServer composites on the 780M once armed (debug.nvaccelfb)\n*\n!WindowServer\n' > /Library/GPUBundles/nvmtl-allow.txt
chmod 644 /Library/GPUBundles/nvmtl-allow.txt && cat /Library/GPUBundles/nvmtl-allow.txt
