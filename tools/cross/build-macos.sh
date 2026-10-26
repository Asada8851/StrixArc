#!/bin/bash
# Cross-builds PhxBringup.kext and mesboot for macOS x86_64 on Linux.
# usage: tools/cross/build-macos.sh <toolchain env.sh> [out dir]
#
# The toolchain env.sh exports PATH/LD_LIBRARY_PATH for clang + ld64.lld, SDKROOT (MacOSX15.5.sdk), MKSDK
# (acidanthera/MacKernelSDK) and LD64 (cctools-port ld64, the only linker that writes MH_KEXT_BUNDLE).
# Without libtapi that ld64 cannot read the SDK's .tbd stubs, so user tools link with ld64.lld instead.
#
# Two things upstream clang does differently from Apple's, both checked against the kext Xcode built:
#  - global destructors become a __cxa_atexit registration even under -mkernel; the kernel has no __cxa_atexit, so
#    PhxBringup.cpp carries its own and runs the destructors from __mod_term_func (undefined symbols then match);
#  - -fno-use-cxa-atexit is passed explicitly (Apple's driver implies it for kexts).
set -eu
ENV=${1:?usage: build-macos.sh <toolchain env.sh> [out dir]}
. "$ENV"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-$ROOT/build/macos}
OBJ=$OUT/obj
rm -rf "$OUT"; mkdir -p "$OBJ" "$OUT/PhxBringup.kext/Contents/MacOS"

K="$ROOT/src/phxbringup"
KCF="-target x86_64-apple-macos11.0 -mkernel -fno-builtin -fno-common -nostdinc -fno-use-cxa-atexit
  -fno-asynchronous-unwind-tables -I$MKSDK/Headers -I$ROOT/src/amd -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE
  -DAPPLE -DNeXT -Os -Wall"
clang -x c $KCF -c "$K/kmod_info.c" -o "$OBJ/kmod_info.o"
clang++ $KCF -std=c++17 -fapple-kext -fno-rtti -fno-exceptions -c "$K/PhxBringup.cpp" -o "$OBJ/PhxBringup.o"
"$LD64" -arch x86_64 -macos_version_min 11.0 -kext -L"$MKSDK/Library/x86_64" -lkmod \
    "$OBJ/kmod_info.o" "$OBJ/PhxBringup.o" -o "$OUT/PhxBringup.kext/Contents/MacOS/PhxBringup" 2>&1 |
    grep -v "directory not found for option '-F" || true
cp "$K/Info.plist" "$OUT/PhxBringup.kext/Contents/Info.plist"
if x86_64-apple-darwin-nm -u "$OUT/PhxBringup.kext/Contents/MacOS/PhxBringup" | grep -q '___cxa_atexit'; then
    echo "kext still imports __cxa_atexit: it would not load"; exit 1
fi

# PhxAccel for macOS 15 (no NM_TAHOE; src/phxaccel/build.sh is the native Tahoe build). Checked against the binary
# setup_metal.sh installed on the 880M: the same strings and symbols, but for __cxa_atexit (kext_atexit.cpp) and the
# fortified strlcpy upstream clang picks by default (-D_FORTIFY_SOURCE=0).
A="$ROOT/src/phxaccel"
mkdir -p "$OUT/PhxAccel.kext/Contents/MacOS"
ACF="$KCF -O2 -std=c++17 -fapple-kext -fno-rtti -fno-exceptions -D_FORTIFY_SOURCE=0 -I$A -Wno-unused-parameter
  -Wno-inconsistent-missing-override -Wno-unused-function -Wno-unused-variable"
clang++ $ACF -I"$A/accel" -I"$A/accel/iofam" -I"$A/accel/re" -DNMX_ACCEL -DPHX_FB -c "$A/accel/nvrm-accel.cpp" -o "$OBJ/accel.o"
clang++ $ACF -c "$A/accel/phxfb.cpp" -o "$OBJ/phxfb.o"
clang++ $ACF -c "$A/accel/phxagdc.cpp" -o "$OBJ/phxagdc.o"
clang++ $ACF -c "$A/accel/kext_atexit.cpp" -o "$OBJ/kext_atexit.o"
"$LD64" -arch x86_64 -macos_version_min 11.0 -kext -L"$MKSDK/Library/x86_64" -lkmod \
    "$OBJ/accel.o" "$OBJ/phxfb.o" "$OBJ/phxagdc.o" "$OBJ/kext_atexit.o" -o "$OUT/PhxAccel.kext/Contents/MacOS/PhxAccel" 2>&1 |
    grep -v "directory not found for option '-F" || true
cp "$A/Info.plist" "$OUT/PhxAccel.kext/Contents/Info.plist"
if x86_64-apple-darwin-nm -u "$OUT/PhxAccel.kext/Contents/MacOS/PhxAccel" | grep -q -E '___cxa_atexit|_chk$'; then
    echo "PhxAccel imports __cxa_atexit or a fortified libc call: it would not load"; exit 1
fi

UCF="-target x86_64-apple-macos12.0 -isysroot $SDKROOT -fuse-ld=lld -Wl,-adhoc_codesign -O2 -Wall"
clang $UCF -I"$ROOT/src/amd" "$ROOT/tools/phxctl/mesboot.c" -framework IOKit -framework CoreFoundation \
    -o "$OUT/mesboot"
echo "built: $OUT/PhxBringup.kext $OUT/PhxAccel.kext $OUT/mesboot"
