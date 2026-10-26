//
//  rdna3_dcn314_scanout.cpp — Implementation of DCN 3.1.4 scanout engine.
//
#include "rdna3_dcn314_scanout.h"

namespace rdna3 {

// DCN 3.1.4 Dword register offsets relative to DMU / DCN base:
constexpr uint32_t regOTG_H_TOTAL                  = 0x0020;
constexpr uint32_t regOTG_H_BLANK_START_END        = 0x0021;
constexpr uint32_t regOTG_H_SYNC_A                 = 0x0022;
constexpr uint32_t regOTG_V_TOTAL                  = 0x0024;
constexpr uint32_t regOTG_V_BLANK_START_END        = 0x0025;
constexpr uint32_t regOTG_V_SYNC_A                 = 0x0026;
constexpr uint32_t regOTG_UPDATE_MODE              = 0x0028;

constexpr uint32_t regDCSURF_SURFACE_PITCH         = 0x0110;
constexpr uint32_t regDCSURF_SURFACE_CONFIG        = 0x0111;
constexpr uint32_t regDCSURF_PRIMARY_SURFACE_ADDR_LO = 0x0112;
constexpr uint32_t regDCSURF_PRIMARY_SURFACE_ADDR_HI = 0x0113;

DisplayTimings Dcn314Scanout::compute_native_timings() {
    DisplayTimings t = {};
    t.h_active          = 2560;
    t.h_front_porch     = 48;
    t.h_sync_width      = 32;
    t.h_back_porch      = 80;
    t.h_total           = t.h_active + t.h_front_porch + t.h_sync_width + t.h_back_porch; // 2720

    t.v_active          = 1600;
    t.v_front_porch     = 3;
    t.v_sync_width      = 6;
    t.v_back_porch      = 39;
    t.v_total           = t.v_active + t.v_front_porch + t.v_sync_width + t.v_back_porch; // 1648

    t.refresh_rate_hz   = 120;
    t.pixel_clock_hz    = static_cast<uint64_t>(t.h_total) * t.v_total * t.refresh_rate_hz; // 537,907,200 Hz

    return t;
}

Dcn314Registers Dcn314Scanout::generate_register_image(const DisplayTimings &t,
                                                       uint64_t fb_mc_addr,
                                                       uint32_t stride_bytes) {
    Dcn314Registers regs = {};

    regs.otg_h_total       = t.h_total - 1;
    regs.otg_h_blank_start = t.h_active;
    regs.otg_h_blank_end   = t.h_total;
    regs.otg_h_sync_start  = t.h_active + t.h_front_porch;
    regs.otg_h_sync_end    = regs.otg_h_sync_start + t.h_sync_width;

    regs.otg_v_total       = t.v_total - 1;
    regs.otg_v_blank_start = t.v_active;
    regs.otg_v_blank_end   = t.v_total;
    regs.otg_v_sync_start  = t.v_active + t.v_front_porch;
    regs.otg_v_sync_end    = regs.otg_v_sync_start + t.v_sync_width;

    regs.dcsurf_surface_pitch   = stride_bytes / 4; // pitch in dwords
    regs.dcsurf_surface_config  = 0x00000004;       // 32-bit ARGB8888 linear
    regs.dcsurf_primary_addr_lo = static_cast<uint32_t>(fb_mc_addr & 0xFFFFFFFF);
    regs.dcsurf_primary_addr_hi = static_cast<uint32_t>(fb_mc_addr >> 32);

    regs.otg_master_update_mode = 0; // immediate update

    return regs;
}

bool Dcn314Scanout::program_scanout(DeviceContext &ctx,
                                    const Dcn314Registers &regs,
                                    uint32_t dcn_base_offset) {
    if (!ctx.mmio) return false;

    // Horizontal timings
    ctx.mmio->write32(dcn_base_offset + regOTG_H_TOTAL, regs.otg_h_total);
    ctx.mmio->write32(dcn_base_offset + regOTG_H_BLANK_START_END,
                      regs.otg_h_blank_start | (regs.otg_h_blank_end << 16));
    ctx.mmio->write32(dcn_base_offset + regOTG_H_SYNC_A,
                      regs.otg_h_sync_start | (regs.otg_h_sync_end << 16));

    // Vertical timings
    ctx.mmio->write32(dcn_base_offset + regOTG_V_TOTAL, regs.otg_v_total);
    ctx.mmio->write32(dcn_base_offset + regOTG_V_BLANK_START_END,
                      regs.otg_v_blank_start | (regs.otg_v_blank_end << 16));
    ctx.mmio->write32(dcn_base_offset + regOTG_V_SYNC_A,
                      regs.otg_v_sync_start | (regs.otg_v_sync_end << 16));

    // Surface & Framebuffer
    ctx.mmio->write32(dcn_base_offset + regDCSURF_SURFACE_PITCH, regs.dcsurf_surface_pitch);
    ctx.mmio->write32(dcn_base_offset + regDCSURF_SURFACE_CONFIG, regs.dcsurf_surface_config);
    ctx.mmio->write32(dcn_base_offset + regDCSURF_PRIMARY_SURFACE_ADDR_LO, regs.dcsurf_primary_addr_lo);
    ctx.mmio->write32(dcn_base_offset + regDCSURF_PRIMARY_SURFACE_ADDR_HI, regs.dcsurf_primary_addr_hi);

    // Apply update
    ctx.mmio->write32(dcn_base_offset + regOTG_UPDATE_MODE, regs.otg_master_update_mode);

    return true;
}

} // namespace rdna3
