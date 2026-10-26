//
//  test_rdna3_chip.cpp — chip profiles (src/amd/rdna3_chip.h): lookup, the Phoenix values the kext and mesboot used
//  before the table existed, the GC 11.5 values from gc_11_5_0_offset.h, and the Phoenix -> Strix translation.
//  tools/check_chip_regs.py checks every table entry against a Linux tree; this test pins the ones the code uses.
//
#include "rdna3_chip.h"
#include <cassert>
#include <cstdio>

extern "C" uint32_t rdna3_chip_c_translate(uint16_t pci_id, uint32_t phx_abs, int *ok);

static const rdna3_chip *P = &rdna3_chip_phoenix, *S = &rdna3_chip_strix_point;

static uint32_t xl(const rdna3_chip *c, uint32_t phx_abs) {
    uint32_t out = 0;
    assert(rdna3_gcvm_translate(c, phx_abs, &out));
    return out;
}

static bool refused(const rdna3_chip *c, uint32_t phx_abs) {
    uint32_t out = 0;
    return !rdna3_gcvm_translate(c, phx_abs, &out);
}

static void test_lookup() {
    assert(rdna3_chip_for_pci(0x1900) == P);
    assert(rdna3_chip_for_pci(0x150E) == S);
    assert(rdna3_chip_for_pci(0x15BF) == nullptr);   // other Phoenix IDs stay unmatched until someone tries them
    assert(rdna3_chip_for_pci(0x7550) == nullptr);
    assert(rdna3_chip_for_gc((11u << 16) | 1u) == P);
    assert(rdna3_chip_for_gc((11u << 16) | (5u << 8)) == S);
    assert(rdna3_chip_for_gc(12u << 16) == nullptr);
    assert(P->verified_on_hardware && !S->verified_on_hardware);
    assert(P->gb_addr_config == 0x242 && S->gb_addr_config == 0x242);   // Mesa's canned phoenix / strix1
    std::puts("[PASS] test_lookup");
}

// The constants PhxBringup.cpp had before the table (kRegCtx0*, kRegInvEng17*) and mesboot's SEG0() literals.
static void test_phoenix_unchanged() {
    const rdna3_gcvm_regs &g = P->gcvm;
    assert(g.ctx0_cntl == 0x28E8 && g.ctx0_pt_base_lo == 0x2953);
    assert(g.ctx0_pt_start_lo == 0x2973 && g.ctx0_pt_end_lo == 0x2993);
    assert(g.inv_eng0_req + 17 == 0x291C && g.inv_eng0_ack + 17 == 0x292E);
    assert(g.inv_eng0_range_lo == 0x292F);
    assert(g.l2_cntl == 0x1260 + 0x15bc && g.mx_l1_tlb_cntl == 0x1260 + 0x167f);
    assert(P->nbio_hdp_remap == 0xE4D);   // PHX_NBIO_REMAP_HDP_MEM_FLUSH_CNTL
    assert(P->hdp_flush_direct == 0xE17);   // NBIF seg2 + BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL
    assert(P->mes.gc[0] == 0x1260 && P->mes.gc[1] == 0xA000 && P->mes.gc[3] == 0);   // what mesboot always sent
    assert(P->mmhub_fb_location_base == 0x1A8EC && P->mmhub_fb_location_top == 0x1A8ED);   // PHX_MMMC_VM_FB_*
    assert(P->mmhub_fb_offset == 0x1A8D7);
    assert(P->otg0.control == 0x34c0 + 0x1b41 && P->otg0.frame_count == 0x34c0 + 0x1b4c);
    for (uint32_t r = 0x2000; r < 0x3000; r++)   // Phoenix translates to itself everywhere
        assert(xl(P, r) == r);
    std::puts("[PASS] test_phoenix_unchanged");
}

static void test_strix_values() {
    const rdna3_gcvm_regs &g = S->gcvm;
    assert(g.ctx0_cntl == 0x28EC && g.ctx0_pt_base_lo == 0x2957);
    assert(g.ctx0_pt_start_lo == 0x2977 && g.ctx0_pt_end_lo == 0x2997);
    assert(g.inv_eng0_req + 17 == 0x2920 && g.inv_eng0_ack + 17 == 0x2932);
    assert(g.fb_location_base == 0x1260 + 0x167c);
    assert(g.sys_aperture_default_lsb == P->gcvm.sys_aperture_default_lsb);   // did not move
    assert(S->nbio_hdp_remap == 0);   // NBIO 7.11 moved it to segment 5, beyond BAR5
    assert(S->hdp_flush_direct == 0xE17);
    // the 880M's ip_discovery: GC / MMHUB / OSSSYS bases 0..3
    assert(S->mes.gc[2] == 0x2402C00 && S->mes.gc[3] == 0x3C02C000 && S->mes.mmhub[3] == 0x3C088000);
    assert(S->mes.osssys[0] == 0x10A0 && S->mes.osssys[2] == 0x3C0A0000);
    assert(S->mmhub_fb_location_base == 0x1A9EC && S->mmhub_fb_offset == 0x1A9D7);   // MMHUB 3.3: +0x100
    assert(S->otg0.h_total == P->otg0.h_total && S->otg0.v_total == P->otg0.v_total);
    assert(S->otg0.control == 0x34c0 + 0x1b43 && S->otg0.frame_count == 0x34c0 + 0x1b4d);
    std::puts("[PASS] test_strix_values");
}

static void test_translate() {
    const uint32_t seg0 = RDNA3_GC_SEG0;
    assert(xl(S, seg0 + 0x15bc) == seg0 + 0x15c0);          // GCVM_L2_CNTL
    assert(xl(S, seg0 + 0x167a) == seg0 + 0x167e);          // GCMC_VM_AGP_TOP
    assert(xl(S, 0x28e8 + 15) == 0x28ec + 15);              // GCVM_CONTEXT15_CNTL
    assert(xl(S, seg0 + 0x16ab + 17) == seg0 + 0x16af + 17);  // INVALIDATE_ENG17_REQ
    assert(xl(S, 0x292f + 2 * 17 + 1) == 0x2933 + 2 * 17 + 1);  // ENG17 range HI
    assert(xl(S, seg0 + 0x16f4) == seg0 + 0x16f8);          // CONTEXT0 PT base HI
    assert(xl(S, 0x2993 + 2 * 15 + 1) == 0x2997 + 2 * 15 + 1);  // CONTEXT15 PT end HI, the window's last register
    // outside the window: the same register on both chips
    assert(xl(S, seg0 + 0x15a7) == seg0 + 0x15a7);          // GCMC_VM_FB_OFFSET
    assert(xl(S, seg0 + 0x15a8) == seg0 + 0x15a8);          // SYSTEM_APERTURE_DEFAULT_ADDR_LSB
    assert(xl(S, 0x263e) == 0x263e);                        // GB_ADDR_CONFIG
    assert(xl(S, 0xA000 + 0x900) == 0xA000 + 0x900);        // GRBM_GFX_CNTL
    assert(xl(S, seg0 + 0x1fab) == seg0 + 0x1fab);          // CP_HQD_ACTIVE
    // inside the window but not in the table: these moved by other amounts on 11.5
    assert(refused(S, seg0 + 0x15b7));                      // GCUTCL2_CGTT_BUSY_CTRL (+1)
    assert(refused(S, seg0 + 0x15e5));                      // GCUTC_GPUVA_VMID_TRANSLATION_ASSIST_REQUEST_LO (+146)
    // same answers from C
    int ok = 0;
    assert(rdna3_chip_c_translate(0x150E, seg0 + 0x15bc, &ok) == seg0 + 0x15c0 && ok);
    assert(rdna3_chip_c_translate(0x1900, seg0 + 0x15bc, &ok) == seg0 + 0x15bc && ok);
    rdna3_chip_c_translate(0x150E, seg0 + 0x15e5, &ok);
    assert(!ok);
    std::puts("[PASS] test_translate");
}

int main() {
    std::puts("=== Running chip profile tests ===");
    test_lookup();
    test_phoenix_unchanged();
    test_strix_values();
    test_translate();
    std::puts("=== All chip profile tests PASSED ===");
    return 0;
}
