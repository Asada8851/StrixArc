//
//  rdna3_dcn314_scanout.h — Direct eDP display scanout engine for DCN 3.1.4 (2560x1600 @ 120Hz).
//  Pure C++17 freestanding, validated against Firebat U6C 16-inch panel.
//
#pragma once
#include "rdna3_ip_blocks.h"
#include <stdint.h>

namespace rdna3 {

struct DisplayTimings {
    uint32_t h_active;
    uint32_t h_front_porch;
    uint32_t h_sync_width;
    uint32_t h_back_porch;
    uint32_t h_total;

    uint32_t v_active;
    uint32_t v_front_porch;
    uint32_t v_sync_width;
    uint32_t v_back_porch;
    uint32_t v_total;

    uint32_t refresh_rate_hz;
    uint64_t pixel_clock_hz;
};

// DCN 3.1.4 Register Image for OTG and HUBP
struct Dcn314Registers {
    uint32_t otg_h_total;
    uint32_t otg_h_blank_start;
    uint32_t otg_h_blank_end;
    uint32_t otg_h_sync_start;
    uint32_t otg_h_sync_end;

    uint32_t otg_v_total;
    uint32_t otg_v_blank_start;
    uint32_t otg_v_blank_end;
    uint32_t otg_v_sync_start;
    uint32_t otg_v_sync_end;

    uint32_t dcsurf_surface_pitch;
    uint32_t dcsurf_surface_config;
    uint32_t dcsurf_primary_addr_lo;
    uint32_t dcsurf_primary_addr_hi;

    uint32_t otg_master_update_mode;
};

class Dcn314Scanout {
public:
    // Calculates standard reduced-blanking eDP timings for 2560x1600 @ 120Hz
    static DisplayTimings compute_native_timings();

    // Generates OTG and HUBP register values for the panel and framebuffer
    static Dcn314Registers generate_register_image(const DisplayTimings &timings,
                                                   uint64_t fb_mc_addr,
                                                   uint32_t stride_bytes);

    // Programs DCN 3.1.4 registers via MMIO
    static bool program_scanout(DeviceContext &ctx,
                                const Dcn314Registers &regs,
                                uint32_t dcn_base_offset);
};

} // namespace rdna3
