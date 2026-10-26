/*
 * rdna3_chip.h — per-chip constants for the GFX11 APUs nullarc can drive.
 *
 * Phoenix / Hawk Point (Radeon 780M/760M, GC 11.0.1) is the reference chip. Strix Point (Radeon 880M/890M,
 * GC 11.5.0) follows the same Linux code paths (gfx_v11_0, imu_v11_0, mes_v11_0, psp_v13_0 for PSP 14.0.0,
 * the same SMU mailbox and EnableGfxImu message) but its GC register map moved: the GCVM/GCMC block sits 4 dwords
 * higher (gc_11_5_0_offset.h), and DCN 3.5 moved a few OTG registers. Everything here is taken from the Linux
 * headers named next to it; tools/check_chip_regs.py re-checks the table against a Linux tree.
 *
 * Plain C (C99) so the kext, phxctl/mesboot and the unit tests can all include it. Register values are absolute
 * MMIO dword offsets (IP segment base + register offset), as wreg()/rreg() take them.
 */
#ifndef RDNA3_CHIP_H
#define RDNA3_CHIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IP base segments. Phoenix's come from hardware/dumps/firebat_u6c/ip_discovery.txt. Strix Point is assumed to use
 * the same ones (MP0/MP1 0x16000, MMHUB 0x1A000, SMUIO 0x16A00, GC, DCN) until a Strix ip_discovery dump confirms
 * it: docs/strix-point.md. */
#define RDNA3_GC_SEG0 0x1260u   /* GC base segment 0 on both chips (Phoenix ip_discovery; Strix: to be confirmed) */
#define RDNA3_GC_SEG1 0xA000u
#define RDNA3_DCN_SEG2 0x34C0u  /* DCN base segment 2 (Phoenix ip_discovery; Strix: to be confirmed) */
#define RDNA3_MMHUB_SEG1 0x1A000u  /* MMHUB base segment 1 (Phoenix ip_discovery; Strix: to be confirmed) */

enum rdna3_chip_id {
    RDNA3_CHIP_UNKNOWN = 0,
    RDNA3_CHIP_PHOENIX = 1,      /* GC 11.0.1, PSP 13.0.4, SMU 13.0.4, SDMA 6.0.1, DCN 3.1.4 */
    RDNA3_CHIP_STRIX_POINT = 2,  /* GC 11.5.0, PSP 14.0.0, SMU 14.0.0, SDMA 6.1.0, DCN 3.5.0 */
};

/* GCVM / GCMC registers the GART, the per-VMID page tables and the TLB flushes use. GCMC_VM_FB_OFFSET (0x15a7) and
 * the SYSTEM_APERTURE_DEFAULT_ADDR pair did not move on 11.5; the rest of the block is 4 dwords higher. */
struct rdna3_gcvm_regs {
    uint32_t l2_cntl, l2_cntl2, l2_cntl3, l2_cntl4, l2_cntl5;
    uint32_t l2_prot_fault_cntl, l2_prot_fault_cntl2, l2_prot_fault_status;
    uint32_t l2_prot_fault_addr_lo, l2_prot_fault_addr_hi;
    uint32_t l2_prot_fault_default_lo, l2_prot_fault_default_hi;
    uint32_t l2_ctx1_identity_low_lo, l2_ctx1_identity_low_hi, l2_ctx1_identity_high_lo;
    uint32_t agp_top, agp_bot, agp_base;
    uint32_t sys_aperture_low, sys_aperture_high;
    uint32_t sys_aperture_default_lsb, sys_aperture_default_msb;   /* unmoved on 11.5 */
    uint32_t mx_l1_tlb_cntl;
    uint32_t fb_location_base, fb_location_top;   /* VRAM MC range, in 16 MiB units */
    uint32_t ctx0_cntl;               /* + vmid */
    uint32_t inv_eng0_req;            /* + engine */
    uint32_t inv_eng0_ack;            /* + engine */
    uint32_t inv_eng0_range_lo;       /* + 2 * engine (HI at +1) */
    uint32_t ctx0_pt_base_lo;         /* + 2 * vmid (HI at +1) */
    uint32_t ctx0_pt_start_lo;        /* + 2 * vmid */
    uint32_t ctx0_pt_end_lo;          /* + 2 * vmid */
};

/* OTG0 registers PhxAMDFB reads to learn the panel's refresh rate. */
struct rdna3_otg_regs {
    uint32_t h_total, v_total, control, frame_count;
};

/* MES SET_HW_RESOURCES gc_base / mmhub_base / osssys_base: the first five IP discovery bases of GC, MMHUB and OSSSYS
 * (Linux adev->reg_offset[..][0][0..4]). Strix Point's are from its ip_discovery dump
 * (tools/check_strix_discovery.py prints them). All zero = not known, and mesboot will not start the scheduler. */
struct rdna3_mes_bases {
    uint32_t gc[5], mmhub[5], osssys[5];
};

struct rdna3_chip {
    enum rdna3_chip_id id;
    const char *name;            /* short name: phoenix, strix-point */
    uint16_t pci_device_id;      /* vendor 0x1002 */
    uint32_t gc_version;         /* (major << 16) | (minor << 8) | rev, as phxn_info.gc_version */
    uint32_t gb_addr_config;     /* fallback only: the kext reads the live GB_ADDR_CONFIG */
    const char *model;           /* IORegistry "model" (About This Mac) */
    const char *mesa_canned;     /* RADV_DARWIN_CANNED entry in Mesa's amdgpu_devices.c */
    const char *fw_gc;           /* linux-firmware prefix for pfp/me/mec/rlc/imu/mes */
    const char *fw_sdma;
    const char *fw_psp_toc;
    uint32_t nbio_hdp_remap;     /* BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL (absolute), 0 = not known for this chip */
    /* BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL (absolute): writing 0 flushes HDP directly, used when the remap
     * register above is unknown or implausible. NBIF segment 2 (0xd20) + 0xf7 on NBIO 7.7 and 7.11 alike. */
    uint32_t hdp_flush_direct;
    /* MMMC_VM_FB_LOCATION_BASE/TOP and MMMC_VM_FB_OFFSET: where the carve-out sits (MMHUB segment 1) */
    uint32_t mmhub_fb_location_base, mmhub_fb_location_top, mmhub_fb_offset;
    struct rdna3_gcvm_regs gcvm;
    struct rdna3_otg_regs otg0;
    struct rdna3_mes_bases mes;
    int verified_on_hardware;    /* 0: built from the Linux headers only, never run */
};

#define RDNA3_GCVM_(o) (RDNA3_GC_SEG0 + (o))
#define RDNA3_OTG_(o) (RDNA3_DCN_SEG2 + (o))

/* gc_11_0_0_offset.h (Phoenix uses the GC 11.0.0 register map); dcn_3_1_4_offset.h */
static const struct rdna3_chip rdna3_chip_phoenix = {
    RDNA3_CHIP_PHOENIX, "phoenix", 0x1900, (11u << 16) | (0u << 8) | 1u, 0x00000242,
    "AMD Radeon 780M", "phoenix", "gc_11_0_1", "sdma_6_0_1.bin", "psp_13_0_4_toc.bin",
    0x0d20u + 0x012du,           /* nbio_7_7_0: seg2 0x12d, NBIF seg2 base 0xd20 */
    0x0d20u + 0x00f7u,           /* nbio_7_7_0 BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL */
    RDNA3_MMHUB_SEG1 + 0x08ecu, RDNA3_MMHUB_SEG1 + 0x08edu, RDNA3_MMHUB_SEG1 + 0x08d7u,   /* mmhub_3_0_1 */
    {
        RDNA3_GCVM_(0x15bc), RDNA3_GCVM_(0x15bd), RDNA3_GCVM_(0x15be), RDNA3_GCVM_(0x15d4), RDNA3_GCVM_(0x15da),
        RDNA3_GCVM_(0x15c4), RDNA3_GCVM_(0x15c5), RDNA3_GCVM_(0x15c8),
        RDNA3_GCVM_(0x15c9), RDNA3_GCVM_(0x15ca),
        RDNA3_GCVM_(0x15cb), RDNA3_GCVM_(0x15cc),
        RDNA3_GCVM_(0x15ce), RDNA3_GCVM_(0x15cf), RDNA3_GCVM_(0x15d0),
        RDNA3_GCVM_(0x167a), RDNA3_GCVM_(0x167b), RDNA3_GCVM_(0x167c),
        RDNA3_GCVM_(0x167d), RDNA3_GCVM_(0x167e),
        RDNA3_GCVM_(0x15a8), RDNA3_GCVM_(0x15a9),
        RDNA3_GCVM_(0x167f),
        RDNA3_GCVM_(0x1678), RDNA3_GCVM_(0x1679),
        RDNA3_GCVM_(0x1688),
        RDNA3_GCVM_(0x16ab),
        RDNA3_GCVM_(0x16bd),
        RDNA3_GCVM_(0x16cf),
        RDNA3_GCVM_(0x16f3),
        RDNA3_GCVM_(0x1713),
        RDNA3_GCVM_(0x1733),
    },
    { RDNA3_OTG_(0x1b2a), RDNA3_OTG_(0x1b2f), RDNA3_OTG_(0x1b41), RDNA3_OTG_(0x1b4c) },
    { { 0x1260, 0xA000, 0x2402C00, 0, 0 }, { 0x13200, 0x1A000, 0x2408800, 0, 0 }, { 0x10A0, 0x240A000, 0, 0, 0 } },
    1,
};

/* gc_11_5_0_offset.h; dcn_3_5_0_offset.h. Device 0x150E covers both the 880M and the 890M (they differ in
 * revision and active CUs). */
static const struct rdna3_chip rdna3_chip_strix_point = {
    RDNA3_CHIP_STRIX_POINT, "strix-point", 0x150E, (11u << 16) | (5u << 8) | 0u, 0x00000242,
    "AMD Radeon 880M/890M", "strix1", "gc_11_5_0", "sdma_6_1_0.bin", "psp_14_0_0_toc.bin",
    0,                           /* nbio_7_11_0 moved it to seg5 0x8e4d (absolute 0x4048e4d): beyond BAR5 */
    0x0d20u + 0x00f7u,           /* nbio_7_11_0 BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL (seg2 0xf7, as Linux's PF1 regs) */
    RDNA3_MMHUB_SEG1 + 0x09ecu, RDNA3_MMHUB_SEG1 + 0x09edu, RDNA3_MMHUB_SEG1 + 0x09d7u,   /* mmhub_3_3_0 (MMHUB 3.3.x) */
    {
        RDNA3_GCVM_(0x15c0), RDNA3_GCVM_(0x15c1), RDNA3_GCVM_(0x15c2), RDNA3_GCVM_(0x15d8), RDNA3_GCVM_(0x15de),
        RDNA3_GCVM_(0x15c8), RDNA3_GCVM_(0x15c9), RDNA3_GCVM_(0x15cc),
        RDNA3_GCVM_(0x15cd), RDNA3_GCVM_(0x15ce),
        RDNA3_GCVM_(0x15cf), RDNA3_GCVM_(0x15d0),
        RDNA3_GCVM_(0x15d2), RDNA3_GCVM_(0x15d3), RDNA3_GCVM_(0x15d4),
        RDNA3_GCVM_(0x167e), RDNA3_GCVM_(0x167f), RDNA3_GCVM_(0x1680),
        RDNA3_GCVM_(0x1681), RDNA3_GCVM_(0x1682),
        RDNA3_GCVM_(0x15a8), RDNA3_GCVM_(0x15a9),
        RDNA3_GCVM_(0x1683),
        RDNA3_GCVM_(0x167c), RDNA3_GCVM_(0x167d),
        RDNA3_GCVM_(0x168c),
        RDNA3_GCVM_(0x16af),
        RDNA3_GCVM_(0x16c1),
        RDNA3_GCVM_(0x16d3),
        RDNA3_GCVM_(0x16f7),
        RDNA3_GCVM_(0x1717),
        RDNA3_GCVM_(0x1737),
    },
    { RDNA3_OTG_(0x1b2a), RDNA3_OTG_(0x1b2f), RDNA3_OTG_(0x1b43), RDNA3_OTG_(0x1b4d) },
    /* the 880M's ip_discovery (2026-10-25): GC, MMHUB, OSSSYS instance 0 bases 0..4 */
    { { 0x1260, 0xA000, 0x2402C00, 0x3C02C000, 0 }, { 0x13200, 0x1A000, 0x2408800, 0x3C088000, 0 },
      { 0x10A0, 0x240A000, 0x3C0A0000, 0, 0 } },
    0,
};

/* The profile for a PCI device ID (vendor 0x1002), or NULL for a GPU nullarc does not know. */
static inline const struct rdna3_chip *rdna3_chip_for_pci(uint16_t device_id) {
    switch (device_id) {
    case 0x1900: return &rdna3_chip_phoenix;   /* the reference machine; other Phoenix IDs are untested */
    case 0x150E: return &rdna3_chip_strix_point;
    default: return NULL;
    }
}

/* The profile for a phxn_info.gc_version value (what userspace sees from the kext). */
static inline const struct rdna3_chip *rdna3_chip_for_gc(uint32_t gc_version) {
    if (gc_version == rdna3_chip_phoenix.gc_version) return &rdna3_chip_phoenix;
    if (gc_version == rdna3_chip_strix_point.gc_version) return &rdna3_chip_strix_point;
    return NULL;
}

/* Registers per field of struct rdna3_gcvm_regs, in field order: 1 for a single register, otherwise the indexed
 * range the field starts (16 VMIDs, 18 invalidation engines, LO/HI pairs per VMID or engine). */
static const uint8_t rdna3_gcvm_span[] = {
    1, 1, 1, 1, 1,  1, 1, 1,  1, 1,  1, 1,  1, 1, 1,  1, 1, 1,  1, 1,  1, 1,  1,  1, 1,
    16, 18, 18, 36, 32, 32, 32,
};
typedef char rdna3_gcvm_span_matches_struct[
    (sizeof(rdna3_gcvm_span) == sizeof(struct rdna3_gcvm_regs) / sizeof(uint32_t)) ? 1 : -1];

/* Phoenix's GCVM window: the absolute offsets whose Strix Point address differs (gc_11_0_0 0x15b6..0x1752). */
#define RDNA3_PHX_GCVM_FIRST (RDNA3_GC_SEG0 + 0x15b6u)
#define RDNA3_PHX_GCVM_LAST  (RDNA3_GC_SEG0 + 0x1752u)

/* Maps a register address written in Phoenix (GC 11.0) terms to the same register on `chip`. Addresses outside
 * the GCVM window are the same on both chips and pass through. Returns 0 for an address inside the window that the
 * table does not cover: the caller must not guess (several registers there moved by other amounts on 11.5). */
static inline int rdna3_gcvm_translate(const struct rdna3_chip *chip, uint32_t phx_abs, uint32_t *out) {
    if (chip == NULL || chip->id == RDNA3_CHIP_PHOENIX || phx_abs < RDNA3_PHX_GCVM_FIRST ||
        phx_abs > RDNA3_PHX_GCVM_LAST) {
        *out = phx_abs;
        return 1;
    }
    const uint32_t *from = (const uint32_t *)&rdna3_chip_phoenix.gcvm, *to = (const uint32_t *)&chip->gcvm;
    for (size_t i = 0; i < sizeof(rdna3_gcvm_span); i++) {
        if (phx_abs >= from[i] && phx_abs < from[i] + rdna3_gcvm_span[i]) {
            *out = to[i] + (phx_abs - from[i]);
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* RDNA3_CHIP_H */
