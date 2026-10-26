#!/bin/bash
# On the Mac (root): move the Metal plugin and its Vulkan stack out of /Library/GPUBundles into
# /System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU and leave symlinks behind. Sandboxed GPU
# processes (WebKit's com.apple.WebKit.GPU, Chromium/Electron helpers) may map executables only from system paths
# (com.apple.WebKit.GPUProcess.sb allows file-map-executable in /System/Library/CoreServices, but /Library/GPUBundles
# only for reading); that CoreTypes directory is a firmlink into the writable Data volume, so it passes the check.
# The originals stay in /Library/GPUBundles-backup (running processes keep their mapped copies). Idempotent.
set -eu
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU; G=/Library/GPUBundles; B=/Library/GPUBundles-backup
mkdir -p "$H" "$B"
for n in NVMTLDriver.bundle nvmtl; do
    [ -L "$G/$n" ] && { echo "$n: already a link to $(readlink "$G/$n")"; continue; }
    rm -rf "$H/$n"; cp -Rp "$G/$n" "$H/"
    rm -rf "$B/$n"; mv "$G/$n" "$B/$n"
    ln -s "$H/$n" "$G/$n"
    echo "$n -> $H/$n"
done
codesign --verify --strict "$H/NVMTLDriver.bundle" && echo "plugin signature ok"
