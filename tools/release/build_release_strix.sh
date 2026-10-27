#!/bin/bash
# StrixArc release package, built on Linux from a test-round folder the reference machine ran (its binaries exactly as
# tested: PhxBringup, PhxAccel, NVMTLDriver, RADV, mesboot/phxctl/fwplan), the 9 firmware files from linux-firmware
# (unmodified, LICENSE.amdgpu) and the licenses of a repository checkout:
#   tools/release/build_release_strix.sh <version> <round folder> <repo checkout> [out dir] [linux-firmware amdgpu dir]
# Leaves <out>/StrixArc-<version>.zip, PhxBringup.kext-<version>.zip and SHA256SUMS. Not in it: NullMoth's bundle and its
# AIR translator (setup_metal.sh takes them from NullMoth 2.4.1's package), Apple binaries, the round's own NOTE.
set -euo pipefail
V=$1; SRC=$(cd "$2" && pwd); R=$(cd "$3" && pwd); OUT=$(mkdir -p "${4:-$PWD}" && cd "${4:-$PWD}" && pwd)
LF=${5:-/usr/lib/firmware/amdgpu}
FW="psp_14_0_0_toc gc_11_5_0_pfp gc_11_5_0_me gc_11_5_0_mec gc_11_5_0_mes_2 gc_11_5_0_mes1 gc_11_5_0_imu gc_11_5_0_rlc sdma_6_1_0"
P=$OUT/StrixArc-$V
rm -rf "$P"; mkdir -p "$P"
for d in EFI-Kexts metal gpu tools; do cp -R "$SRC/$d" "$P/"; done
for f in BUILD HZ120; do cp "$SRC/$f" "$P/"; done
# the installers from the checkout (the round folder carries the same ones; install.sh also takes gpu/fw)
cp "$R/tools/phxctl/go.sh" "$P/go.sh"
for f in install install_boot setup_metal update_accel; do cp "$R/tools/phxctl/${f}_strix.sh" "$P/$f.sh"; done
printf 'gpu/hz_ws_check.sh\ngpu/fps_check.sh\n' > "$P/TESTS"
echo "$V" > "$P/ROUND"
mkdir -p "$P/gpu/fw"
for f in $FW; do
    if [ -f "$LF/$f.bin.zst" ]; then zstd -qdcf "$LF/$f.bin.zst" > "$P/gpu/fw/$f.bin"; else cp "$LF/$f.bin" "$P/gpu/fw/"; fi
done
cp "$R/LICENSE" "$R/THIRD_PARTY_NOTICES.md" "$R/README.md" "$R/README_RU.md" "$P/"
cp -R "$R/LICENSES" "$P/"
cat > "$P/README.txt" <<TXT
StrixArc $V - Metal on the AMD Radeon 880M (Strix Point) for macOS Sequoia 15, 3072x1920 at 120 Hz

Tested on ONE machine only (the author's Lenovo laptop, Radeon 880M, macOS 15.8.1, OpenCore). No guarantee it works
anywhere else, or at all. Read README.md first (README_RU.md in Russian): what it does, how to get back.

  EFI-Kexts/PhxBringup.kext   goes into OpenCore's EFI/OC/Kexts (add it to config.plist; UEFI > Output > Resolution = Max)
  install.sh                  tools, scripts and firmware into ~/gpu (no sudo)
  setup_metal.sh              once: sudo bash setup_metal.sh <NullMoth 2.4.1 pkgroot> .   (the Metal plugin, RADV, PhxAccel)
  go.sh                       then: bash go.sh - installs the boot daemon (WindowServer on the GPU, 120 Hz at boot),
                              says when to reboot; after the reboot run it again: the tests turn SDMA copies at boot on
  gpu/fw                      linux-firmware's amdgpu files, unmodified (LICENSES/LICENSE.amdgpu.txt)
  LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/
                              parts derived from NullMoth's driver are for noncommercial use only (PolyForm Noncommercial)

https://github.com/Asada8851/StrixArc
TXT
chmod 755 "$P"/*.sh "$P"/gpu/*.sh "$P"/gpu/phxctl/* "$P"/metal/mtlprobe "$P"/metal/fpsanim 2>/dev/null || true
cd "$OUT"
rm -f "StrixArc-$V.zip" "PhxBringup.kext-$V.zip"
# zip if there is one, else bsdtar (libarchive) writing zip; both keep the execute bits
mkzip() { if command -v zip >/dev/null; then zip -qr -X "$1" "$2"; else bsdtar --format zip -cf "$1" "$2"; fi; }
mkzip "StrixArc-$V.zip" "StrixArc-$V"
(cd "$P/EFI-Kexts" && mkzip "$OUT/PhxBringup.kext-$V.zip" PhxBringup.kext)
sha256sum "StrixArc-$V.zip" "PhxBringup.kext-$V.zip" > SHA256SUMS
ls -la "StrixArc-$V.zip" "PhxBringup.kext-$V.zip"; cat SHA256SUMS
