//
//  rdna3_ucode_extract.h — Decoder and payload extractor for RDNA 3 / GFX11 firmware.
//  Pure C++17, freestanding, bounds-checked.
//
#pragma once
#include "rdna3_ucode_psp.h"
#include <stdint.h>

namespace rdna3 {

constexpr uint32_t kMaxUcodePayloadsPerFile = 16;

struct UcodePayload {
    uint32_t fw_type;       // PSPGfxFwType value
    uint32_t offset_bytes;  // Byte offset from start of .bin file
    uint32_t size_bytes;    // Size of payload
};

// Host firmware IDs passed to rdna3_ucode_extract:
constexpr uint64_t kHostFwIPBase       = 0x100;
constexpr uint64_t kHostFwFile_SDMA    = 0x200 + 0;
constexpr uint64_t kHostFwFile_RLC     = 0x200 + 1;
constexpr uint64_t kHostFwFile_IMU     = 0x200 + 2;
constexpr uint64_t kHostFwFile_MES     = 0x200 + 3;   // scheduler pipe (gc_*_mes_2.bin)
constexpr uint64_t kHostFwFile_CP_PFP  = 0x200 + 4;
constexpr uint64_t kHostFwFile_CP_ME   = 0x200 + 5;
constexpr uint64_t kHostFwFile_CP_MEC  = 0x200 + 6;
constexpr uint64_t kHostFwFile_DMCUB   = 0x200 + 7;
constexpr uint64_t kHostFwFile_PSP_TA  = 0x200 + 8;
constexpr uint64_t kHostFwFile_MES_KIQ = 0x200 + 9;   // KIQ pipe (gc_*_mes1.bin)

// Extracts payload entries from raw firmware binary.
// Returns count of payloads written to out[] (0 on invalid / corrupted file).
uint32_t rdna3_ucode_extract(uint64_t hostFwType,
                             const uint8_t *bin,
                             uint64_t size_bytes,
                             UcodePayload out[kMaxUcodePayloadsPerFile]);

} // namespace rdna3
