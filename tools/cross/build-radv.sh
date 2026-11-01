#!/bin/bash
# Cross-builds RADV (libvulkan_radeon.dylib) for macOS x86_64 on Linux.
#   tools/cross/build-radv.sh <toolchain env.sh> <mesa tree> [build dir]
# The tree is Mesa f5cb8ee032ad + Cobalt64's mesa-patches 0001..0009 (github.com/Almosst-DEV/Cobalt64, MIT) +
# mesa-patches-phoenix 0001..; the options are mesa-patches-phoenix/README.txt's Mac recipe plus the strix1 default.
# MESA_VIDEO_CODECS=h264dec,h265dec adds the decoders Mesa leaves out by default (the deployed RADV has neither).
# Needs meson >= 1.4, ninja, glslangValidator, and Python with mako/pyyaml/packaging (MESA_PY=<venv>/bin to use one).
set -eu
ENV=${1:?usage: build-radv.sh <env.sh> <mesa tree> [build dir]}; MESA=$(cd "${2:?mesa tree}" && pwd)
. "$ENV"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=${3:-$MESA/build-darwin}
[ -n "${MESA_PY:-}" ] && export PATH="$MESA_PY:$PATH"
X=$B.ini; mkdir -p "$(dirname "$B")"
sed -e "s|@NX@|$NX|g" -e "s|@SDK@|$SDKROOT|g" "$ROOT/tools/cross/mesa-darwin-x86_64.ini" > "$X"
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$MESA" --cross-file "$X" --buildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
        -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
        -Dgles2=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
        -Dbuild-tests=false -Dvulkan-layers= -Dtools= \
        ${MESA_VIDEO_CODECS:+-Dvideo-codecs=$MESA_VIDEO_CODECS}
fi
ninja -C "$B" src/amd/vulkan/libvulkan_radeon.dylib
ls -la "$B/src/amd/vulkan/libvulkan_radeon.dylib"
