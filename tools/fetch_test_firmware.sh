#!/usr/bin/env bash
# Download the firmware blobs used by tests/test_rdna3_ucode.cpp from upstream
# linux-firmware: Phoenix (Radeon 780M) by default, Strix Point (Radeon 880M/890M)
# with "strix" as the second argument. These files are redistributable under
# AMD's LICENSE.amdgpu but are not committed to this repository.
#   fetch_test_firmware.sh [dest] [phoenix|strix]
set -euo pipefail

DEST="${1:-tests/fixtures/phoenix}"
CHIP="${2:-phoenix}"
# Mirrors are tried in order; override with LINUX_FIRMWARE_URL=<base url of amdgpu/>.
if [ -n "${LINUX_FIRMWARE_URL:-}" ]; then
  MIRRORS=("$LINUX_FIRMWARE_URL")
else
  MIRRORS=(
    "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/amdgpu"
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/amdgpu"
  )
fi
case "$CHIP" in
  phoenix) FILES=(
    gc_11_0_1_rlc.bin
    gc_11_0_1_pfp.bin
    gc_11_0_1_me.bin
    gc_11_0_1_mec.bin
    gc_11_0_1_imu.bin
    gc_11_0_1_mes_2.bin
    gc_11_0_1_mes1.bin
    sdma_6_0_1.bin
    dcn_3_1_4_dmcub.bin
  ) ;;
  strix) FILES=(
    gc_11_5_0_rlc.bin
    gc_11_5_0_pfp.bin
    gc_11_5_0_me.bin
    gc_11_5_0_mec.bin
    gc_11_5_0_imu.bin
    gc_11_5_0_mes_2.bin
    gc_11_5_0_mes1.bin
    sdma_6_1_0.bin
    psp_14_0_0_toc.bin
  ) ;;
  *) echo "usage: $0 [dest] [phoenix|strix]" >&2; exit 2 ;;
esac

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
  if [ -s "$DEST/$f" ]; then
    echo "[fixtures] $f already present"
    continue
  fi
  ok=0
  for base in "${MIRRORS[@]}"; do
    echo "[fixtures] fetching $f from $base"
    if curl -fsSL --retry 3 -A "nullarc-ci" -o "$DEST/$f.tmp" "$base/$f"; then
      # Reject HTML (bot challenge / error pages) masquerading as firmware.
      if head -c 512 "$DEST/$f.tmp" | grep -qiE '<(!doctype|html)'; then
        echo "[fixtures]   got an HTML page instead of firmware, trying next mirror" >&2
      else
        mv "$DEST/$f.tmp" "$DEST/$f"; ok=1; break
      fi
    else
      echo "[fixtures]   download failed, trying next mirror" >&2
    fi
    rm -f "$DEST/$f.tmp"
  done
  if [ "$ok" != 1 ]; then
    echo "[fixtures] ERROR: could not download $f from any mirror" >&2
    exit 1
  fi
done
echo "[fixtures] done: $DEST"
