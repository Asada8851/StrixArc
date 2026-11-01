#!/bin/bash
# Cross-builds FFmpeg (shared libraries + ffmpeg/ffprobe/ffplay) for macOS x86_64 with Vulkan Video decode, and vkshim,
# the one-driver stand-in for the Vulkan loader that hands FFmpeg StrixArc's RADV.
# usage: tools/cross/build-ffmpeg.sh <toolchain env.sh> <ffmpeg source dir> [out dir]
#   SDL2_ROOT=<dir>: SDL2 for ffplay (default build/sdl2-macos/root, from tools/cross/build-sdl2.sh)
#
# Why our own FFmpeg: Homebrew's is built without Vulkan (and Homebrew no longer serves Intel Macs). ffplay is the
# player: ffmpeg-patches/ lets it decode on VCN without its Vulkan renderer, copying frames to memory for SDL's Metal
# renderer. The source is copied and patched under the out dir; the given tree stays as released.
# The majors are FFmpeg 8.x's (avcodec 62, avutil 60), so other programs built against 8.x can run on these libraries
# through DYLD_LIBRARY_PATH. Only FFmpeg's internal codecs: no external libraries but what macOS has, SDL2 linked in.
# iconv is libiconv on macOS, not libSystem (configure's link test passes without it, the libraries do not).
# Install names are @rpath/...; the CLI finds them in ../lib, other programs through DYLD_LIBRARY_PATH.
#
# vkshim is installed as lib/libvulkan.dylib only - the name FFmpeg dlopens first - and not as libvulkan.1.dylib, the
# name programs link the real loader under (mpv/libplacebo with MoltenVK): theirs stays the real one.
set -eu
ENV=${1:?usage: build-ffmpeg.sh <toolchain env.sh> <ffmpeg source dir> [out dir]}
SRC=$(cd "${2:?ffmpeg source dir}" && pwd)
. "$ENV"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${3:-$ROOT/build/ffmpeg-macos}
BLD=$OUT/build
SDL2_ROOT=${SDL2_ROOT:-$ROOT/build/sdl2-macos/root}
[ -f "$SDL2_ROOT/lib/pkgconfig/sdl2.pc" ] || { echo "no SDL2 in $SDL2_ROOT (tools/cross/build-sdl2.sh)"; exit 1; }
rm -rf "$OUT"; mkdir -p "$BLD" "$OUT/lib"
cp -a "$SRC" "$OUT/src"
for p in "$ROOT"/ffmpeg-patches/*.patch; do
    patch -d "$OUT/src" -p1 --quiet < "$p" || { echo "patch failed: $p"; exit 1; }
done
SRC=$OUT/src

# the Vulkan headers from the build host (vulkan-headers), alone next to the macOS SDK, as for vkvideo
VKH=$BLD/vkinclude; mkdir -p "$VKH"
for d in vulkan vk_video; do [ -d /usr/include/$d ] && ln -sfn /usr/include/$d "$VKH/$d"; done
[ -e "$VKH/vulkan/vulkan.h" ] || { echo "no /usr/include/vulkan (vulkan-headers)"; exit 1; }
command -v nasm >/dev/null || { echo "no nasm"; exit 1; }

CC="clang -target x86_64-apple-macos12.0 -isysroot $SDKROOT"
cd "$BLD"
# pkg-config sees SDL2 alone (the host's .pc files are Linux libraries)
PKG_CONFIG_LIBDIR="$SDL2_ROOT/lib/pkgconfig" PKG_CONFIG_PATH= "$SRC/configure" --prefix=/ --enable-cross-compile --target-os=darwin --arch=x86_64 \
    --cc="$CC" --cxx="clang++ -target x86_64-apple-macos12.0 -isysroot $SDKROOT" --objcc="$CC" --ld="$CC -fuse-ld=lld" \
    --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=x86_64-apple-darwin-strip --x86asmexe=nasm \
    --pkg-config=pkg-config --pkg-config-flags=--static --extra-cflags="-I$VKH" --extra-ldflags="-Wl,-adhoc_codesign" --extra-libs=-liconv \
    --install-name-dir=@rpath --extra-ldsoflags="-Wl,-rpath,@loader_path" --extra-ldexeflags="-Wl,-rpath,@executable_path/../lib" \
    --enable-shared --disable-static --disable-doc --disable-debug --disable-autodetect \
    --enable-vulkan --enable-videotoolbox --enable-audiotoolbox --enable-zlib --enable-bzlib --enable-iconv \
    --enable-securetransport --enable-sdl2 --enable-ffplay \
    > "$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; tail -40 "$BLD/ffbuild/config.log"; exit 1; }
grep -E '^(CONFIG_VULKAN|CONFIG_H264_VULKAN_HWACCEL|CONFIG_HEVC_VULKAN_HWACCEL|HAVE_X86ASM|CONFIG_VIDEOTOOLBOX|CONFIG_SDL2|CONFIG_FFPLAY)=' \
    "$BLD/ffbuild/config.mak" | tee -a "$OUT/configure.log"
grep -q '^CONFIG_HEVC_VULKAN_HWACCEL=yes' "$BLD/ffbuild/config.mak" || { echo "configure left Vulkan decode out"; exit 1; }
grep -q '^CONFIG_FFPLAY=yes' "$BLD/ffbuild/config.mak" || { echo "configure left ffplay out (SDL2?)"; exit 1; }
make -j"$(nproc)" > "$OUT/make.log" 2>&1 || { tail -40 "$OUT/make.log"; exit 1; }
make install DESTDIR="$OUT/root" > "$OUT/install.log" 2>&1 || { tail -20 "$OUT/install.log"; exit 1; }
mkdir -p "$OUT/bin"
cp "$OUT/root/bin/ffmpeg" "$OUT/root/bin/ffprobe" "$OUT/root/bin/ffplay" "$OUT/bin/"
# the libraries: the real files under their major names (what dyld looks for), no symlink chains on a FAT32 stick
for f in "$OUT"/root/lib/lib*.dylib; do
    [ -L "$f" ] && continue
    n=$(basename "$f"); m=$(echo "$n" | sed -E 's/^(lib[a-z]+\.[0-9]+)\..*\.dylib$/\1.dylib/')
    cp "$f" "$OUT/lib/$m"
done
rm -rf "$OUT/root" "$OUT/src"

clang -target x86_64-apple-macos12.0 -isysroot "$SDKROOT" -fuse-ld=lld -Wl,-adhoc_codesign -O2 -Wall -I"$VKH" \
    -dynamiclib -install_name @rpath/libvulkan.dylib "$ROOT/tools/vkshim/vkshim.c" -o "$OUT/lib/libvulkan.dylib"
echo "built: $OUT/bin/{ffmpeg,ffprobe,ffplay} $OUT/lib/"; ls "$OUT/lib"
