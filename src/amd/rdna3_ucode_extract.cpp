//
//  rdna3_ucode_extract.cpp — Decoder and payload extractor for RDNA 3 / GFX1103 firmware.
//  Pure C++17, freestanding, bounds-checked.
//
#include "rdna3_ucode_extract.h"
#include <string.h>

namespace rdna3 {

namespace {

// Safe bounds check: verifies that [offset, offset + size) lies completely within file_size.
// Guarded against uint64_t overflow.
inline bool within(uint64_t offset, uint64_t size, uint64_t file_size) {
    if (size == 0) return false;
    if (offset >= file_size) return false;
    if (offset + size > file_size) return false;
    if (offset + size < offset) return false; // overflow guard
    return true;
}

// Appends a payload to out[] if valid and capacity allows.
inline uint32_t push(UcodePayload out[], uint32_t count,
                     uint32_t fw_type,
                     uint64_t offset, uint64_t size,
                     uint64_t file_size) {
    if (count >= kMaxUcodePayloadsPerFile) return count;
    if (!within(offset, size, file_size)) return count;
    out[count].fw_type      = fw_type;
    out[count].offset_bytes = static_cast<uint32_t>(offset);
    out[count].size_bytes   = static_cast<uint32_t>(size);
    return count + 1;
}

// SDMA extractor (Supports v2.0 used on Phoenix / GFX1103, and v3.0 used on RDNA 4).
uint32_t extract_sdma(const uint8_t *bin, uint64_t size, UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *common = reinterpret_cast<const common_firmware_header *>(bin);

    if (common->header_version_major == 2) {
        if (size < sizeof(sdma_firmware_header_v2_0)) return 0;
        auto *hdr = reinterpret_cast<const sdma_firmware_header_v2_0 *>(bin);
        uint32_t n = 0;
        // Thread 0: Context thread ucode
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SDMA_UCODE_TH0),
                 hdr->header.ucode_array_offset_bytes,
                 hdr->ctx_ucode_size_bytes, size);
        // Thread 1: Control thread ucode
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SDMA_UCODE_TH1),
                 hdr->ctl_ucode_offset,
                 hdr->ctl_ucode_size_bytes, size);
        return n;
    } else if (common->header_version_major == 3) {
        if (size < sizeof(sdma_firmware_header_v3_0)) return 0;
        auto *hdr = reinterpret_cast<const sdma_firmware_header_v3_0 *>(bin);
        return push(out, 0, static_cast<uint32_t>(PSPGfxFwType::SDMA_UCODE_TH0),
                    hdr->header.ucode_array_offset_bytes,
                    hdr->ucode_size_bytes, size);
    }

    return 0;
}

// RLC extractor (Supports v2.0 - v2.4).
// Order matches upstream Linux kernel amdgpu_rlc.c.
// RLC_G must be pushed LAST because loading it triggers psp_rlc_autoload_start.
uint32_t extract_rlc(const uint8_t *bin, uint64_t size, UcodePayload out[]) {
    if (size < sizeof(rlc_firmware_header_v2_0)) return 0;
    auto *hv2_0 = reinterpret_cast<const rlc_firmware_header_v2_0 *>(bin);
    uint16_t maj = hv2_0->header.header_version_major;
    uint16_t mnr = hv2_0->header.header_version_minor;
    if (maj != 2) return 0;

    uint32_t n = 0;

    // v2.4 tap delays
    if (mnr >= 4 && size >= sizeof(rlc_firmware_header_v2_4)) {
        auto *hv2_4 = reinterpret_cast<const rlc_firmware_header_v2_4 *>(bin);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::GLOBAL_TAP_DELAYS),
                 hv2_4->global_tap_delays_ucode_offset_bytes,
                 hv2_4->global_tap_delays_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SE0_TAP_DELAYS),
                 hv2_4->se0_tap_delays_ucode_offset_bytes,
                 hv2_4->se0_tap_delays_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SE1_TAP_DELAYS),
                 hv2_4->se1_tap_delays_ucode_offset_bytes,
                 hv2_4->se1_tap_delays_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SE2_TAP_DELAYS),
                 hv2_4->se2_tap_delays_ucode_offset_bytes,
                 hv2_4->se2_tap_delays_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::SE3_TAP_DELAYS),
                 hv2_4->se3_tap_delays_ucode_offset_bytes,
                 hv2_4->se3_tap_delays_ucode_size_bytes, size);
    }

    // v2.1 save/restore lists
    if (mnr >= 1 && size >= sizeof(rlc_firmware_header_v2_1)) {
        auto *hv2_1 = reinterpret_cast<const rlc_firmware_header_v2_1 *>(bin);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL),
                 hv2_1->save_restore_list_cntl_offset_bytes,
                 hv2_1->save_restore_list_cntl_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM),
                 hv2_1->save_restore_list_gpm_offset_bytes,
                 hv2_1->save_restore_list_gpm_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM),
                 hv2_1->save_restore_list_srm_offset_bytes,
                 hv2_1->save_restore_list_srm_size_bytes, size);
    }

    // v2.2 IRAM + DRAM
    if (mnr >= 2 && size >= sizeof(rlc_firmware_header_v2_2)) {
        auto *hv2_2 = reinterpret_cast<const rlc_firmware_header_v2_2 *>(bin);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_IRAM),
                 hv2_2->rlc_iram_ucode_offset_bytes,
                 hv2_2->rlc_iram_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_DRAM_BOOT),
                 hv2_2->rlc_dram_ucode_offset_bytes,
                 hv2_2->rlc_dram_ucode_size_bytes, size);
    }

    // v2.3 RLC_P + RLC_V
    if (mnr >= 3 && size >= sizeof(rlc_firmware_header_v2_3)) {
        auto *hv2_3 = reinterpret_cast<const rlc_firmware_header_v2_3 *>(bin);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_P),
                 hv2_3->rlcp_ucode_offset_bytes,
                 hv2_3->rlcp_ucode_size_bytes, size);
        n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_V),
                 hv2_3->rlcv_ucode_offset_bytes,
                 hv2_3->rlcv_ucode_size_bytes, size);
    }

    // v2.0 RLC_G LAST
    n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::RLC_G),
             hv2_0->header.ucode_array_offset_bytes,
             hv2_0->header.ucode_size_bytes, size);

    return n;
}

// IMU extractor (IMU_I + IMU_D).
uint32_t extract_imu(const uint8_t *bin, uint64_t size, UcodePayload out[]) {
    if (size < sizeof(imu_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const imu_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;

    uint32_t n = 0;
    n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::IMU_I),
             hdr->header.ucode_array_offset_bytes,
             hdr->imu_iram_ucode_size_bytes, size);
    // IMU_D begins right after IMU_I in memory
    uint64_t dram_offset = static_cast<uint64_t>(hdr->header.ucode_array_offset_bytes) +
                           hdr->imu_iram_ucode_size_bytes;
    n = push(out, n, static_cast<uint32_t>(PSPGfxFwType::IMU_D),
             dram_offset,
             hdr->imu_dram_ucode_size_bytes, size);
    return n;
}

// MES extractor: the ucode and its data (stack) section, typed for the scheduler or the KIQ pipe.
uint32_t extract_mes(const uint8_t *bin, uint64_t size, bool kiq, UcodePayload out[]) {
    if (size < sizeof(mes_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;

    uint32_t n = 0;
    n = push(out, n, static_cast<uint32_t>(kiq ? PSPGfxFwType::CP_MES_KIQ : PSPGfxFwType::CP_MES),
             hdr->mes_ucode_offset_bytes, hdr->mes_ucode_size_bytes, size);
    n = push(out, n, static_cast<uint32_t>(kiq ? PSPGfxFwType::MES_KIQ_STACK : PSPGfxFwType::MES_STACK),
             hdr->mes_ucode_data_offset_bytes, hdr->mes_ucode_data_size_bytes, size);
    return n;
}

// CP family (PFP / ME / MEC) for GFX11 (RDNA 3): F32 (v1, Phoenix) or RS64 (v2).
// GFX11 RS64 stack layout:
//   PFP: P0, P1 (2 stacks)
//   ME:  P0, P1 (2 stacks)
//   MEC: P0, P1, P2, P3 (4 stacks)
enum class RS64Family { PFP, ME, MEC };

uint32_t extract_cp_rs64(const uint8_t *bin, uint64_t size,
                         RS64Family family,
                         UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *common = reinterpret_cast<const common_firmware_header *>(bin);

    // Header v1.x: legacy F32 CP microcode, as shipped for Phoenix (GC 11.0.1)
    // in linux-firmware. One payload, loaded by PSP as CP_PFP / CP_ME / CP_MEC
    // (gfx_v11_0 with rs64_enable == false; no stacks and no MEC jump tables).
    if (common->header_version_major == 1) {
        uint32_t legacy_type = 0;
        switch (family) {
        case RS64Family::PFP: legacy_type = static_cast<uint32_t>(PSPGfxFwType::CP_PFP); break;
        case RS64Family::ME:  legacy_type = static_cast<uint32_t>(PSPGfxFwType::CP_ME);  break;
        case RS64Family::MEC: legacy_type = static_cast<uint32_t>(PSPGfxFwType::CP_MEC); break;
        }
        return push(out, 0, legacy_type,
                    common->ucode_array_offset_bytes,
                    common->ucode_size_bytes, size);
    }

    // Header v2.x: RS64 CP microcode (Navi 3x) with per-pipe stacks.
    if (common->header_version_major != 2) return 0;
    if (size < sizeof(gfx_firmware_header_v2_0)) return 0;
    auto *hdr = reinterpret_cast<const gfx_firmware_header_v2_0 *>(bin);

    uint32_t n = 0;
    uint32_t ucode_fw_type = 0;
    uint32_t stack_fw_types[4] = {0, 0, 0, 0};
    uint32_t num_stacks = 0;

    switch (family) {
    case RS64Family::PFP:
        ucode_fw_type     = static_cast<uint32_t>(PSPGfxFwType::RS64_PFP);
        stack_fw_types[0] = static_cast<uint32_t>(PSPGfxFwType::RS64_PFP_P0_STACK);
        stack_fw_types[1] = static_cast<uint32_t>(PSPGfxFwType::RS64_PFP_P1_STACK);
        num_stacks = 2;
        break;
    case RS64Family::ME:
        ucode_fw_type     = static_cast<uint32_t>(PSPGfxFwType::RS64_ME);
        stack_fw_types[0] = static_cast<uint32_t>(PSPGfxFwType::RS64_ME_P0_STACK);
        stack_fw_types[1] = static_cast<uint32_t>(PSPGfxFwType::RS64_ME_P1_STACK);
        num_stacks = 2;
        break;
    case RS64Family::MEC:
        ucode_fw_type     = static_cast<uint32_t>(PSPGfxFwType::RS64_MEC);
        stack_fw_types[0] = static_cast<uint32_t>(PSPGfxFwType::RS64_MEC_P0_STACK);
        stack_fw_types[1] = static_cast<uint32_t>(PSPGfxFwType::RS64_MEC_P1_STACK);
        stack_fw_types[2] = static_cast<uint32_t>(PSPGfxFwType::RS64_MEC_P2_STACK);
        stack_fw_types[3] = static_cast<uint32_t>(PSPGfxFwType::RS64_MEC_P3_STACK);
        num_stacks = 4;
        break;
    }

    // Main ucode payload
    n = push(out, n, ucode_fw_type,
             hdr->header.ucode_array_offset_bytes,
             hdr->ucode_size_bytes, size);

    // Per-pipe stack copies (same data, distinct FW types)
    for (uint32_t i = 0; i < num_stacks; ++i) {
        n = push(out, n, stack_fw_types[i],
                 hdr->data_offset_bytes,
                 hdr->data_size_bytes, size);
    }

    return n;
}

// DMCUB extractor (DCN display micro-controller).
uint32_t extract_dmcub(const uint8_t *bin, uint64_t size, UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *common = reinterpret_cast<const common_firmware_header *>(bin);
    return push(out, 0, static_cast<uint32_t>(PSPGfxFwType::DMUB),
                common->ucode_array_offset_bytes,
                common->ucode_size_bytes, size);
}

// TA (Trusted Application) v2 extractor.
uint32_t extract_psp_ta(const uint8_t *bin, uint64_t size, UcodePayload out[]) {
    if (size < sizeof(ta_firmware_header_v2_0)) return 0;
    auto *hdr = reinterpret_cast<const ta_firmware_header_v2_0 *>(bin);
    if (hdr->header.header_version_major != 2) return 0;

    uint32_t count = hdr->ta_fw_bin_count;
    if (count > kMaxUcodePayloadsPerFile) count = kMaxUcodePayloadsPerFile;

    // Verify descriptor array fits in file
    uint64_t descs_size = sizeof(ta_firmware_header_v2_0) + (uint64_t)count * sizeof(psp_fw_bin_desc);
    if (descs_size > size) return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &desc = hdr->ta_fw_bin[i];
        uint64_t payload_offset = (uint64_t)hdr->header.ucode_array_offset_bytes + desc.offset_bytes;
        n = push(out, n, desc.fw_type, payload_offset, desc.size_bytes, size);
    }
    return n;
}

// Fallback for single-payload IP firmware using the common header (e.g. SMU / PMFW).
uint32_t extract_common_header(uint32_t psp_fw_type,
                               const uint8_t *bin, uint64_t size,
                               UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *hdr = reinterpret_cast<const common_firmware_header *>(bin);
    return push(out, 0, psp_fw_type,
                hdr->ucode_array_offset_bytes,
                hdr->ucode_size_bytes, size);
}

} // anonymous namespace

uint32_t rdna3_ucode_extract(uint64_t hostFwType,
                             const uint8_t *bin,
                             uint64_t size_bytes,
                             UcodePayload out[kMaxUcodePayloadsPerFile]) {
    if (bin == nullptr || size_bytes == 0 || out == nullptr) return 0;
    memset(out, 0, sizeof(UcodePayload) * kMaxUcodePayloadsPerFile);

    // Multi-payload / typed file dispatch
    switch (hostFwType) {
    case kHostFwFile_SDMA:
        return extract_sdma(bin, size_bytes, out);
    case kHostFwFile_RLC:
        return extract_rlc(bin, size_bytes, out);
    case kHostFwFile_IMU:
        return extract_imu(bin, size_bytes, out);
    case kHostFwFile_MES:
        return extract_mes(bin, size_bytes, false, out);
    case kHostFwFile_MES_KIQ:
        return extract_mes(bin, size_bytes, true, out);
    case kHostFwFile_CP_PFP:
        return extract_cp_rs64(bin, size_bytes, RS64Family::PFP, out);
    case kHostFwFile_CP_ME:
        return extract_cp_rs64(bin, size_bytes, RS64Family::ME, out);
    case kHostFwFile_CP_MEC:
        return extract_cp_rs64(bin, size_bytes, RS64Family::MEC, out);
    case kHostFwFile_DMCUB:
        return extract_dmcub(bin, size_bytes, out);
    case kHostFwFile_PSP_TA:
        return extract_psp_ta(bin, size_bytes, out);
    default:
        break;
    }

    // Direct / legacy 0x100 + psp_fw_type fallback (SMU, individual loads)
    if (hostFwType >= kHostFwIPBase && hostFwType < 0x200) {
        uint32_t psp_fw_type = static_cast<uint32_t>(hostFwType - kHostFwIPBase);
        return extract_common_header(psp_fw_type, bin, size_bytes, out);
    }

    return 0;
}

} // namespace rdna3
