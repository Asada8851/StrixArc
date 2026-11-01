#!/bin/bash
# Cross-builds SDL2 for macOS x86_64 on Linux, static, for ffplay (tools/cross/build-ffmpeg.sh links it in: one
# binary, nothing more to ship). Cocoa windows, the Metal renderer, CoreAudio - what SDL itself picks on macOS.
# usage: tools/cross/build-sdl2.sh <toolchain env.sh> <SDL2 source dir> [out dir]
# The source: an SDL2 release tarball (checked against its .sig, Sam Lantinga's key 1528635D8053A57F77D1E08630A59377A7763BE6).
# sdl-patches/ are applied to a copy under the out dir; the given tree stays as released.
set -eu
ENV=${1:?usage: build-sdl2.sh <toolchain env.sh> <SDL2 source dir> [out dir]}
SRC=$(cd "${2:?SDL2 source dir}" && pwd)
. "$ENV"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${3:-$ROOT/build/sdl2-macos}
rm -rf "$OUT"; mkdir -p "$OUT/build"
cp -a "$SRC" "$OUT/src"
for p in "$ROOT"/sdl-patches/*.patch; do
    [ -e "$p" ] || continue
    patch -d "$OUT/src" -p1 --quiet < "$p" || { echo "patch failed: $p"; exit 1; }
done
SRC=$OUT/src
T="x86_64-apple-macos12.0"
cat > "$OUT/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_OSX_SYSROOT "$SDKROOT")
set(CMAKE_OSX_ARCHITECTURES x86_64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET $T)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET $T)
set(CMAKE_OBJC_COMPILER clang)
set(CMAKE_OBJC_COMPILER_TARGET $T)
set(CMAKE_AR "$(command -v llvm-ar)")
set(CMAKE_RANLIB "$(command -v llvm-ranlib)")
set(CMAKE_INSTALL_NAME_TOOL "$(command -v x86_64-apple-darwin-install_name_tool)")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH "$SDKROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
cmake -S "$SRC" -B "$OUT/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$OUT/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OUT/root" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    > "$OUT/cmake.log" 2>&1 || { tail -40 "$OUT/cmake.log"; exit 1; }
ninja -C "$OUT/build" install > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
# @available in SDL's Cocoa code needs __isPlatformVersionAtLeast, which upstream clang leaves to a runtime library it
# does not have for macOS: ours goes into libSDL2.a, so whatever links SDL gets it
clang -target $T -isysroot "$SDKROOT" -O2 -Wall -c "$ROOT/tools/cross/availability.c" -o "$OUT/build/availability.o"
llvm-ar r "$OUT/root/lib/libSDL2.a" "$OUT/build/availability.o"
grep -E 'SDL_(COCOA|METAL|RENDER_METAL|COREAUDIO|VIDEO_DRIVER_COCOA|AUDIO_DRIVER_COREAUDIO|VIDEO_RENDER_METAL) ' \
    "$OUT/root/include/SDL2/SDL_config.h" | head
echo "built: $OUT/root (lib/libSDL2.a, lib/pkgconfig/sdl2.pc)"
