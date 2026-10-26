#!/bin/bash
# Installs NVMTLDriver.bundle (NullMoth 2.4.1, unmodified) + our RADV into /Library/GPUBundles for PhxAccel.
set -u
GB=/Library/GPUBundles; S=/Users/Shared/gpu/nmstage
mkdir -p $GB/nvmtl
rm -rf $GB/NVMTLDriver.bundle && ditto $S/NVMTLDriver.bundle $GB/NVMTLDriver.bundle
cp -a $S/nvmtl/. $GB/nvmtl/
cp /Users/Shared/gpu/mesa/build/src/amd/vulkan/libvulkan_radeon.dylib $GB/nvmtl/libvulkan_radeon.dylib
codesign --force --sign - $GB/nvmtl/libvulkan_radeon.dylib 2>&1 | tail -1
cat > $GB/nvmtl/radeon_icd.json <<'J'
{ "file_format_version": "1.0.1", "ICD": { "library_path": "/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib", "library_arch": "64", "api_version": "1.4.363" } }
J
cp $S/nvmtl-allow.txt $GB/nvmtl-allow.txt
chown -R root:wheel $GB/NVMTLDriver.bundle $GB/nvmtl $GB/nvmtl-allow.txt
chmod -R go-w,a+rX $GB/NVMTLDriver.bundle $GB/nvmtl && chmod 644 $GB/nvmtl-allow.txt
mkdir -p $GB/nvmtl/aircache $GB/nvmtl/spvcache && chown -R _windowserver:wheel $GB/nvmtl/aircache $GB/nvmtl/spvcache && chmod 0755 $GB/nvmtl/aircache $GB/nvmtl/spvcache
codesign --verify --strict $GB/NVMTLDriver.bundle 2>&1 | tail -1; echo "plugin installed: $(ls $GB/nvmtl | tr '\n' ' ')"
