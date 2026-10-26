//
//  rdna3_ucode_psp.h — RDNA3 / GFX1103 firmware descriptor structures.
//  Derived directly from Linux kernel drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h
//  and drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h.
//
#pragma once
#include <stdint.h>

namespace rdna3 {

// ---- Common AMD firmware header (v1.0 / v2.0) --------------------------
struct common_firmware_header {
    uint32_t size_bytes;                // Total file size
    uint32_t header_size_bytes;         // Header struct size
    uint16_t header_version_major;      // 1 or 2
    uint16_t header_version_minor;
    uint16_t ip_version_major;          // 11 for GFX11, 13 for PSP13
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;          // Payload size
    uint32_t ucode_array_offset_bytes;  // Offset from start of file to payload
    uint32_t crc32;
} __attribute__((packed));
static_assert(sizeof(common_firmware_header) == 32, "common_firmware_header must be 32 bytes");

// ---- PSP v2 typed descriptors (Used by PSP v13 / RDNA 3) -----------------
struct psp_fw_bin_desc {
    uint32_t fw_type;        // psp_fw_type enum
    uint32_t fw_version;
    uint32_t offset_bytes;   // Relative to ucode_array_offset_bytes
    uint32_t size_bytes;
} __attribute__((packed));
static_assert(sizeof(psp_fw_bin_desc) == 16, "psp_fw_bin_desc must be 16 bytes");

enum psp_fw_type {
    PSP_FW_TYPE_UNKNOWN             = 0,
    PSP_FW_TYPE_PSP_SOS             = 1,
    PSP_FW_TYPE_PSP_SYS_DRV         = 2,
    PSP_FW_TYPE_PSP_KDB             = 3,
    PSP_FW_TYPE_PSP_TOC             = 4,
    PSP_FW_TYPE_PSP_SPL             = 5,
    PSP_FW_TYPE_PSP_RL              = 6,
    PSP_FW_TYPE_PSP_SOC_DRV         = 7,
    PSP_FW_TYPE_PSP_INTF_DRV        = 8,
    PSP_FW_TYPE_PSP_DBG_DRV         = 9,
    PSP_FW_TYPE_PSP_RAS_DRV         = 10,
    PSP_FW_TYPE_PSP_IPKEYMGR_DRV    = 11,
    PSP_FW_TYPE_PSP_SPDM_DRV        = 12,
    PSP_FW_TYPE_MAX_INDEX           = 13
};

struct psp_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t               psp_fw_bin_count;
    psp_fw_bin_desc        psp_fw_bin[];
} __attribute__((packed));

// ---- TA (TrustZone App) binary descriptor ------------------------------
enum ta_fw_type {
    TA_FW_TYPE_UNKNOWN              = 0,
    TA_FW_TYPE_PSP_ASD             = 1,
    TA_FW_TYPE_PSP_XGMI            = 2,
    TA_FW_TYPE_PSP_RAS             = 3,
    TA_FW_TYPE_PSP_HDCP            = 4,
    TA_FW_TYPE_PSP_DTM             = 5,
    TA_FW_TYPE_PSP_RAP             = 6,
    TA_FW_TYPE_PSP_SECUREDISPLAY   = 7,
    TA_FW_TYPE_PSP_XGMI_AUX        = 8,
    TA_FW_TYPE_MAX_INDEX           = 9
};

struct ta_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t               ta_fw_bin_count;
    psp_fw_bin_desc        ta_fw_bin[];
} __attribute__((packed));

// ---- GFX RS64 CP header (v2.0) -------------------------------------------
struct gfx_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ucode_size_bytes;
    uint32_t ucode_offset_bytes;
    uint32_t data_size_bytes;
    uint32_t data_offset_bytes;
    uint32_t ucode_start_addr_lo;
    uint32_t ucode_start_addr_hi;
} __attribute__((packed));
static_assert(sizeof(gfx_firmware_header_v2_0) == 60, "gfx_firmware_header_v2_0 must be 60 bytes");

// ---- RLC firmware headers (GFX11 / RDNA 3) -------------------------------
struct rlc_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t jt_offset;
    uint32_t jt_size;
    uint32_t save_and_restore_offset;
    uint32_t clear_state_descriptor_offset;
    uint32_t avail_scratch_ram_locations;
    uint32_t reg_restore_list_size;
    uint32_t reg_list_format_start;
    uint32_t reg_list_format_separate_start;
    uint32_t starting_offsets_start;
    uint32_t reg_list_format_size_bytes;
    uint32_t reg_list_format_array_offset_bytes;
    uint32_t reg_list_size_bytes;
    uint32_t reg_list_array_offset_bytes;
    uint32_t reg_list_format_separate_size_bytes;
    uint32_t reg_list_format_separate_array_offset_bytes;
    uint32_t reg_list_separate_size_bytes;
    uint32_t reg_list_separate_array_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_0) == 104, "rlc_firmware_header_v2_0 must be 104 bytes");

struct rlc_firmware_header_v2_1 {
    rlc_firmware_header_v2_0 v2_0;
    uint32_t reg_list_format_direct_reg_list_length;
    uint32_t save_restore_list_cntl_ucode_ver;
    uint32_t save_restore_list_cntl_feature_ver;
    uint32_t save_restore_list_cntl_size_bytes;
    uint32_t save_restore_list_cntl_offset_bytes;
    uint32_t save_restore_list_gpm_ucode_ver;
    uint32_t save_restore_list_gpm_feature_ver;
    uint32_t save_restore_list_gpm_size_bytes;
    uint32_t save_restore_list_gpm_offset_bytes;
    uint32_t save_restore_list_srm_ucode_ver;
    uint32_t save_restore_list_srm_feature_ver;
    uint32_t save_restore_list_srm_size_bytes;
    uint32_t save_restore_list_srm_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_1) == 156, "rlc_firmware_header_v2_1 must be 156 bytes");

struct rlc_firmware_header_v2_2 {
    rlc_firmware_header_v2_1 v2_1;
    uint32_t rlc_iram_ucode_size_bytes;
    uint32_t rlc_iram_ucode_offset_bytes;
    uint32_t rlc_dram_ucode_size_bytes;
    uint32_t rlc_dram_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_2) == 172, "rlc_firmware_header_v2_2 must be 172 bytes");

struct rlc_firmware_header_v2_3 {
    rlc_firmware_header_v2_2 v2_2;
    uint32_t rlcp_ucode_version;
    uint32_t rlcp_ucode_feature_version;
    uint32_t rlcp_ucode_size_bytes;
    uint32_t rlcp_ucode_offset_bytes;
    uint32_t rlcv_ucode_version;
    uint32_t rlcv_ucode_feature_version;
    uint32_t rlcv_ucode_size_bytes;
    uint32_t rlcv_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_3) == 204, "rlc_firmware_header_v2_3 must be 204 bytes");

struct rlc_firmware_header_v2_4 {
    rlc_firmware_header_v2_3 v2_3;
    uint32_t global_tap_delays_ucode_size_bytes;
    uint32_t global_tap_delays_ucode_offset_bytes;
    uint32_t se0_tap_delays_ucode_size_bytes;
    uint32_t se0_tap_delays_ucode_offset_bytes;
    uint32_t se1_tap_delays_ucode_size_bytes;
    uint32_t se1_tap_delays_ucode_offset_bytes;
    uint32_t se2_tap_delays_ucode_size_bytes;
    uint32_t se2_tap_delays_ucode_offset_bytes;
    uint32_t se3_tap_delays_ucode_size_bytes;
    uint32_t se3_tap_delays_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_4) == 244, "rlc_firmware_header_v2_4 must be 244 bytes");

// ---- IMU firmware header (v1.0) ------------------------------------------
struct imu_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t imu_iram_ucode_size_bytes;
    uint32_t imu_iram_ucode_offset_bytes;
    uint32_t imu_dram_ucode_size_bytes;
    uint32_t imu_dram_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(imu_firmware_header_v1_0) == 48, "imu_firmware_header_v1_0 must be 48 bytes");

// ---- MES firmware header (v1.0) ------------------------------------------
// gc_*_mes_2.bin is the scheduler pipe (CP_MES + MES_STACK), gc_*_mes1.bin the KIQ pipe (CP_MES_KIQ + MES_KIQ_STACK).
struct mes_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t mes_ucode_version;
    uint32_t mes_ucode_size_bytes;
    uint32_t mes_ucode_offset_bytes;
    uint32_t mes_ucode_data_version;
    uint32_t mes_ucode_data_size_bytes;
    uint32_t mes_ucode_data_offset_bytes;
    uint32_t mes_uc_start_addr_lo;
    uint32_t mes_uc_start_addr_hi;
    uint32_t mes_data_start_addr_lo;
    uint32_t mes_data_start_addr_hi;
} __attribute__((packed));
static_assert(sizeof(mes_firmware_header_v1_0) == 72, "mes_firmware_header_v1_0 must be 72 bytes");

// ---- DMCUB firmware header (v1.0) ----------------------------------------
struct dmcub_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t inst_const_bytes;
    uint32_t bss_data_bytes;
} __attribute__((packed));
static_assert(sizeof(dmcub_firmware_header_v1_0) == 40, "dmcub_firmware_header_v1_0 must be 40 bytes");

// ---- SDMA firmware headers (v2.0 and v3.0) -------------------------------
struct sdma_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ctx_ucode_size_bytes; // context thread ucode size
    uint32_t ctx_jt_offset;        // context thread jt location
    uint32_t ctx_jt_size;          // context thread size of jt
    uint32_t ctl_ucode_offset;     // control thread offset
    uint32_t ctl_ucode_size_bytes; // control thread ucode size
    uint32_t ctx_ctl_jt_offset;    // control thread jt location
    uint32_t ctx_ctl_jt_size;      // control thread size of jt
} __attribute__((packed));
static_assert(sizeof(sdma_firmware_header_v2_0) == 64, "sdma_firmware_header_v2_0 must be 64 bytes");

struct sdma_firmware_header_v3_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ucode_offset_bytes;
    uint32_t ucode_size_bytes;
} __attribute__((packed));
static_assert(sizeof(sdma_firmware_header_v3_0) == 44, "sdma_firmware_header_v3_0 must be 44 bytes");

// ---- PSP GFX FW Type constants for IP loading ----------------------------
// Mirrored from Linux kernel psp_gfx_if.h (enum psp_gfx_fw_type)
enum class PSPGfxFwType : uint32_t {
    CP_ME                       = 1,
    CP_PFP                      = 2,
    CP_CE                       = 3,
    CP_MEC                      = 4,
    CP_MEC_JT1                  = 5,
    CP_MEC_JT2                  = 6,
    RLC_V                       = 7,
    RLC_G                       = 8,
    SDMA0                       = 9,
    SDMA1                       = 10,
    SMU                         = 18,
    RLC_RESTORE_LIST_GPM_MEM    = 20,
    RLC_RESTORE_LIST_SRM_MEM    = 21,
    RLC_RESTORE_LIST_SRM_CNTL   = 22,
    RLC_P                       = 25,
    RLC_IRAM                    = 26,
    GLOBAL_TAP_DELAYS           = 27,
    SE0_TAP_DELAYS              = 28,
    SE1_TAP_DELAYS              = 29,
    CP_MES                      = 33,
    MES_STACK                   = 34,
    RLC_DRAM_BOOT               = 48,
    DMUB                        = 51,
    SE2_TAP_DELAYS              = 65,
    SE3_TAP_DELAYS              = 66,
    REG_LIST                    = 67,
    IMU_I                       = 68,
    IMU_D                       = 69,
    SDMA_UCODE_TH0              = 71, // SDMA Thread 0 / CTX
    SDMA_UCODE_TH1              = 72, // SDMA Thread 1 / CTL
    RS64_MES                    = 76,
    RS64_MES_STACK              = 77,
    RS64_KIQ                    = 78,
    RS64_KIQ_STACK              = 79,
    CP_MES_KIQ                  = 81,
    MES_KIQ_STACK               = 82,
    RS64_PFP                    = 87,
    RS64_ME                     = 88,
    RS64_MEC                    = 89,
    RS64_PFP_P0_STACK           = 90,
    RS64_PFP_P1_STACK           = 91,
    RS64_ME_P0_STACK            = 92,
    RS64_ME_P1_STACK            = 93,
    RS64_MEC_P0_STACK           = 94,
    RS64_MEC_P1_STACK           = 95,
    RS64_MEC_P2_STACK           = 96,
    RS64_MEC_P3_STACK           = 97,
};

} // namespace rdna3
