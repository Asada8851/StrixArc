//
//  rdna3_rebar_uma.cpp — Implementation of pure memory aperture and ReBAR planner.
//
#include "rdna3_rebar_uma.h"

namespace rdna3 {

const char *rebar_status_name(RebarStatus s) {
    switch (s) {
    case RebarStatus::Ok: return "Ok";
    case RebarStatus::LenZero: return "BAR0 length is 0";
    case RebarStatus::LenNotPow2: return "BAR0 length is not power of 2";
    case RebarStatus::AddrZero: return "BAR0 address is 0";
    case RebarStatus::AddrMisaligned: return "BAR0 address is not aligned to length";
    case RebarStatus::AddrWrap: return "BAR0 address + length wraps 64-bit space";
    case RebarStatus::RebarMismatch: return "ReBAR capability size differs from BAR0 length";
    case RebarStatus::NoRebarCapAboveLegacy: return "BAR0 exceeds 256 MiB without ReBAR capability";
    case RebarStatus::VramTooSmall: return "Total VRAM is too small to fit required reserves";
    case RebarStatus::ConsoleBeyondLimit: return "Console extends beyond VRAM limit";
    case RebarStatus::ConsoleOverlap: return "Console overlaps reserved allocation regions";
    case RebarStatus::MapCapZero: return "Mapping cap is 0";
    default: return "Unknown";
    }
}

MemoryPlanOutput plan_memory_aperture(const MemoryPlanInput &in) {
    MemoryPlanOutput out = {};
    out.ok = false;
    out.status = RebarStatus::Ok;

    if (in.map_cap == 0) {
        out.status = RebarStatus::MapCapZero;
        return out;
    }
    if (in.bar0_len == 0) {
        out.status = RebarStatus::LenZero;
        return out;
    }
    if (!is_pow2(in.bar0_len)) {
        out.status = RebarStatus::LenNotPow2;
        return out;
    }
    if (in.bar0_addr == 0) {
        out.status = RebarStatus::AddrZero;
        return out;
    }
    if ((in.bar0_addr & (in.bar0_len - 1ull)) != 0) {
        out.status = RebarStatus::AddrMisaligned;
        return out;
    }
    if (in.bar0_addr + in.bar0_len < in.bar0_addr) {
        out.status = RebarStatus::AddrWrap;
        return out;
    }

    // ReBAR consistency check
    if (in.rebar_present) {
        if (in.rebar_bytes != in.bar0_len) {
            out.status = RebarStatus::RebarMismatch;
            return out;
        }
    } else if (in.bar0_len > kLegacyBar0Size) {
        out.status = RebarStatus::NoRebarCapAboveLegacy;
        return out;
    }

    // Verify VRAM accommodates reserves
    uint64_t total_reserves = in.tmr_reserve + in.scanout_reserve;
    if (in.vram_bytes <= total_reserves) {
        out.status = RebarStatus::VramTooSmall;
        return out;
    }

    uint64_t usable_vram = in.vram_bytes - in.tmr_reserve;
    uint64_t map_len = (in.bar0_len < in.map_cap) ? in.bar0_len : in.map_cap;

    // Determine CPU-visible vram_limit
    uint64_t vram_limit = (map_len < usable_vram) ? map_len : usable_vram;
    vram_limit &= ~(kPageTableAlign - 1ull); // align down to 64 KiB
    if (vram_limit == 0) {
        out.status = RebarStatus::VramTooSmall;
        return out;
    }

    // Console bounds check
    if (in.console_off != ~0ull && in.console_len > 0) {
        if (in.console_off >= vram_limit || in.console_len > (vram_limit - in.console_off)) {
            out.status = RebarStatus::ConsoleBeyondLimit;
            return out;
        }
    }

    out.map_len    = map_len;
    out.vram_limit = vram_limit;

    // Allocator pools:
    // Visible pool starts above scanout reserve (or at scanout reserve)
    out.vis_pool_base = in.scanout_reserve;
    if (vram_limit > out.vis_pool_base) {
        out.vis_pool_size = vram_limit - out.vis_pool_base;
    } else {
        out.vis_pool_size = 0;
    }

    // High pool exists if usable VRAM extends beyond vram_limit
    if (usable_vram > vram_limit) {
        out.has_hi_pool  = true;
        out.hi_pool_base = vram_limit;
        out.hi_pool_size = usable_vram - vram_limit;
    } else {
        out.has_hi_pool  = false;
        out.hi_pool_base = 0;
        out.hi_pool_size = 0;
    }

    // Full ReBAR: entire usable VRAM is CPU-visible without high pool
    out.is_full_rebar = !out.has_hi_pool && (map_len >= usable_vram);

    // Aperture check points (start, midpoint, top minus 64 KiB)
    out.aperture_pts[0] = 0;
    out.aperture_pts[1] = (vram_limit / 2) & ~(kPageTableAlign - 1ull);
    out.aperture_pts[2] = vram_limit - kPageTableAlign;

    out.ok = true;
    out.status = RebarStatus::Ok;
    return out;
}

} // namespace rdna3
