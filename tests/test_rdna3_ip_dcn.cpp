//
//  test_rdna3_ip_dcn.cpp — Unit tests for RDNA 3 IP block state machines and DCN 3.1.4 scanout engine.
//  Builds cleanly under Clang with -fsanitize=address,undefined.
//
#include "../src/amd/rdna3_ip_blocks.h"
#include "../src/amd/rdna3_dcn314_scanout.h"
#include <iostream>
#include <map>
#include <cassert>

using namespace rdna3;

class MockMmioBus : public IMmioBus {
public:
    std::map<uint32_t, uint32_t> regs;

    uint32_t read32(uint32_t dword_offset) override {
        // SMU Response register: if message written, return 1 (Success)
        if (dword_offset == 0x02C2) { // regMP1_C2PMSG_66
            return 1;
        }
        auto it = regs.find(dword_offset);
        if (it != regs.end()) return it->second;
        return 0;
    }

    void write32(uint32_t dword_offset, uint32_t value) override {
        regs[dword_offset] = value;
    }
};

static void test_ip_blocks_bringup() {
    MockMmioBus mock_mmio;
    DeviceContext ctx = {};
    ctx.mmio = &mock_mmio;

    MemoryPlanInput plan_in = {};
    plan_in.is_apu           = true;
    plan_in.bar0_len         = 4ull * kGiB;
    plan_in.bar0_addr        = 0x800000000ull;
    plan_in.bar0_cfg_addr    = 0x800000000ull;
    plan_in.rebar_present    = true;
    plan_in.rebar_bytes      = 4ull * kGiB;
    plan_in.vram_bytes       = 4ull * kGiB;
    plan_in.tmr_reserve      = 64ull * kMiB;
    plan_in.scanout_reserve  = 64ull * kMiB;
    plan_in.map_cap          = 4ull * kGiB;

    auto mem_plan = plan_memory_aperture(plan_in);
    assert(mem_plan.ok);

    bool ok = Rdna3Device::bringup(ctx, mem_plan);
    assert(ok);
    assert(ctx.gmc_ready);
    assert(ctx.psp_online);
    assert(ctx.smu_online);
    assert(ctx.sdma_ready);
    assert(ctx.gfx_ready);

    // Verify MMHUB registers written
    assert(mock_mmio.regs.count(0x0680) > 0); // MMHUB_FB_LOCATION_BASE
    assert(mock_mmio.regs.count(0x0681) > 0); // MMHUB_FB_LOCATION_TOP

    // Verify SDMA & CP rings configured
    assert(mock_mmio.regs.count(0x12A0) > 0); // SDMA0_GFX_RB_CNTL
    assert(mock_mmio.regs.count(0x2400) > 0); // CP_RB0_CNTL
    assert(mock_mmio.regs.count(0x28A0) > 0); // RLC_AUTOLOAD_CNTL

    std::cout << "[PASS] test_ip_blocks_bringup (GMC11, PSP13, SMU13, SDMA6, GFX11 initialized)" << std::endl;
}

static void test_dcn314_scanout_timings() {
    auto t = Dcn314Scanout::compute_native_timings();

    assert(t.h_active == 2560);
    assert(t.v_active == 1600);
    assert(t.refresh_rate_hz == 120);

    assert(t.h_total == 2720);
    assert(t.v_total == 1648);
    assert(t.pixel_clock_hz == 537907200ull);

    uint64_t fb_addr = 0x8000000000ull; // FB at base of MC VRAM
    uint32_t stride  = 2560 * 4;       // 10240 bytes

    auto regs = Dcn314Scanout::generate_register_image(t, fb_addr, stride);

    assert(regs.otg_h_total == 2719);
    assert(regs.otg_v_total == 1647);
    assert(regs.otg_h_blank_start == 2560);
    assert(regs.otg_h_blank_end == 2720);
    assert(regs.otg_v_blank_start == 1600);
    assert(regs.otg_v_blank_end == 1648);
    assert(regs.dcsurf_surface_pitch == 2560);
    assert(regs.dcsurf_surface_config == 0x00000004); // ARGB8888 linear
    assert(regs.dcsurf_primary_addr_lo == 0x00000000);
    assert(regs.dcsurf_primary_addr_hi == 0x00000080);

    // Test register programming via MMIO
    MockMmioBus mock_mmio;
    DeviceContext ctx = {};
    ctx.mmio = &mock_mmio;

    uint32_t dcn_base = 0x4000;
    assert(Dcn314Scanout::program_scanout(ctx, regs, dcn_base));

    // Verify programmed MMIO registers
    assert(mock_mmio.regs[dcn_base + 0x0020] == 2719); // OTG_H_TOTAL
    assert(mock_mmio.regs[dcn_base + 0x0024] == 1647); // OTG_V_TOTAL
    assert(mock_mmio.regs[dcn_base + 0x0110] == 2560); // DCSURF_SURFACE_PITCH
    assert(mock_mmio.regs[dcn_base + 0x0112] == 0x00000000); // PRIMARY_SURFACE_ADDR_LO
    assert(mock_mmio.regs[dcn_base + 0x0113] == 0x00000080); // PRIMARY_SURFACE_ADDR_HI

    std::cout << "[PASS] test_dcn314_scanout_timings (2560x1600@120Hz pixel clock 537.9 MHz exact)" << std::endl;
}

static void test_null_and_error_handling() {
    DeviceContext null_ctx = {};
    null_ctx.mmio = nullptr;

    MemoryPlanOutput invalid_plan = {};
    invalid_plan.ok = false;

    assert(!Gmc11Block::init(null_ctx, invalid_plan));
    assert(!Psp13Block::init(null_ctx));
    assert(!Smu13Block::init(null_ctx));
    assert(!Sdma6Block::init(null_ctx, 0, 0));
    assert(!Gfx11Block::init(null_ctx, 0, 0));
    assert(!Rdna3Device::bringup(null_ctx, invalid_plan));

    std::cout << "[PASS] test_null_and_error_handling" << std::endl;
}

int main() {
    std::cout << "=== Running RDNA3 IP Blocks & DCN 3.1.4 Scanout Tests ===" << std::endl;
    test_ip_blocks_bringup();
    test_dcn314_scanout_timings();
    test_null_and_error_handling();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
