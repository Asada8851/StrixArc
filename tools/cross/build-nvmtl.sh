#!/bin/bash
# Builds the Metal plugin binary (NVMTLDriver, an MH_BUNDLE loaded from NullMoth's NVMTLDriver.bundle) on Linux:
#   tools/cross/build-nvmtl.sh <toolchain env.sh> [out dir]   ->  <out>/NVMTLDriver
# One translation unit (src/nmplugin/NVMTLDevice.m pulls in the rest), ARC, macOS 12+, SDK 15.5 as the earlier builds;
# Like those builds: no constant ObjC literals (NSConstantArray...), no fortified libc calls. Vulkan headers from /usr/include (Arch: vulkan-headers) or VULKAN_INCLUDE; __cpu_model from cpu_model_shim.c.
# NVMTL_RELEASE=1 in the environment builds the release flavour (no debug syslog lines); default is the debug build.
set -euo pipefail
ENV=${1:?usage: build-nvmtl.sh <env.sh> [out dir]}
R=$(cd "$(dirname "$0")/../.." && pwd); OUT=$(mkdir -p "${2:-$R/build/macos}" && cd "${2:-$R/build/macos}" && pwd)
# shellcheck disable=SC1090
source "$ENV"
VK=${VULKAN_INCLUDE:-/usr/include}
[ -f "$VK/vulkan/vulkan_core.h" ] || { echo "no Vulkan headers in $VK (pacman -S vulkan-headers, or VULKAN_INCLUDE=...)"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc"; cp -r "$VK/vulkan" "$T/inc/"; [ -d "$VK/vk_video" ] && cp -r "$VK/vk_video" "$T/inc/"
CF=(-target x86_64-apple-macos12.0 -isysroot "$SDKROOT" -O2 -g0)
clang "${CF[@]}" -I"$T/inc" -fobjc-arc -fmodules -fno-objc-constant-literals -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNVMTL_RELEASE="${NVMTL_RELEASE:-0}" -w \
      -c "$R/src/nmplugin/NVMTLDevice.m" -o "$T/NVMTLDevice.o"
clang "${CF[@]}" -c "$R/tools/cross/cpu_model_shim.c" -o "$T/shim.o"
clang "${CF[@]}" -fuse-ld=lld -bundle -fobjc-arc "$T/NVMTLDevice.o" "$T/shim.o" -framework Foundation -framework CoreFoundation \
      -framework Metal -framework IOSurface -framework IOKit -o "$OUT/NVMTLDriver"
x86_64-apple-darwin-nm -u "$OUT/NVMTLDriver" | grep -q '___cpu_model$' && { echo "__cpu_model still undefined"; exit 1; }
echo "built: $OUT/NVMTLDriver ($(stat -c %s "$OUT/NVMTLDriver") bytes, NVMTL_RELEASE=${NVMTL_RELEASE:-0})"
