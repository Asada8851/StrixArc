#!/bin/bash
# Installs PhxAccel into the Auxiliary Kernel Collection (backup kept in /Users/Shared/gpu/backup) and PhxBringup into EFI.
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
BK=/Users/Shared/gpu/backup; TS=$(date +%Y%m%d-%H%M%S); mkdir -p $BK
cp -p $KC $BK/AuxiliaryKernelExtensions.kc.$TS && echo "backup $BK/AuxiliaryKernelExtensions.kc.$TS"
rm -rf /Library/Extensions/PhxAccel.kext
cp -R /Users/Shared/gpu/phxaccel/build/PhxAccel.kext /Library/Extensions/ && chown -R root:wheel /Library/Extensions/PhxAccel.kext && chmod -R 755 /Library/Extensions/PhxAccel.kext
NEW=$KC.phx-new; rm -f $NEW
kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions -A $NEW -z > /tmp/phx-kmutil.log 2>&1
echo "kmutil rc=$?"
INS=$(kmutil inspect -a x86_64 -A $NEW 2>/dev/null)
for b in com.luuumi.PhxAccel com.highpoint-tech.kext.HighPointIOP com.highpoint-tech.kext.HighPointRR com.apple.driver.AppleMobileDevice; do
  echo "$INS" | grep -q "$b" && echo "  in KC: $b" || { echo "  MISSING: $b -- not publishing"; tail -5 /tmp/phx-kmutil.log; exit 1; }
done
mv -f $NEW $KC && echo "aux KC published"
diskutil mount disk0s1 >/dev/null 2>&1
rm -rf /Volumes/EFI/EFI/OC/Kexts/PhxBringup.kext
cd /Users/Shared/gpu/phxbringup && sh build.sh /Users/Shared/gpu/MacKernelSDK 2>&1 | grep -E " error|built" ; cd build && codesign --force --sign - PhxBringup.kext >/dev/null 2>&1
cp -R /Users/Shared/gpu/phxbringup/build/PhxBringup.kext /Volumes/EFI/EFI/OC/Kexts/ && echo "PhxBringup deployed: $(strings /Volumes/EFI/EFI/OC/Kexts/PhxBringup.kext/Contents/MacOS/PhxBringup | grep -c 'claimed for PhxAccel') claim string"
