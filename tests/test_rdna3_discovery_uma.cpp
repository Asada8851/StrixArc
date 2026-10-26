//
//  test_rdna3_discovery_uma.cpp — Unit tests for IP discovery parser and Phoenix UMA ReBAR planner.
//  Builds cleanly under Clang with -fsanitize=address,undefined.
//
#include "../src/amd/rdna3_ip_discovery.h"
#include "../src/amd/rdna3_rebar_uma.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>

using namespace rdna3;

// Helper to synthesize a valid IP discovery binary containing Phoenix APU IPs
static std::vector<uint8_t> create_synthetic_phoenix_ip_discovery() {
    // Layout:
    // 0x00: binary_header (16 bytes):
    //   u32 sig (0x28211407), u16 maj (2), u16 min (0), u16 checksum, u16 bin_size (total)
    //   u16 num_tables (1), u16 pad (0)
    // 0x10: table_info[0] (8 bytes):
    //   u16 offset (0x20), u16 checksum (0), u16 size (120), u16 pad (0)
    // 0x20: ip_discovery_header (20 bytes + die_info[16] * 4 bytes):
    //   u32 sig ("IPDS" = 0x53445049), u16 version (3), u16 size (84), u32 id (0), u16 num_dies (1), u16 pad
    //   die_info[0]: u16 die_id (0), u16 die_offset (0x80)
    // 0x80: die_header (4 bytes):
    //   u16 die_id (0), u16 num_ips (4)
    // 0x84+: IP entries:
    //   GC:   hw_id=11,  inst=0, nbase=2, maj=11, min=0, rev=3, sub=0, bases=[0x1000, 0x2000] (8+8 = 16 bytes)
    //   MP0:  hw_id=255, inst=0, nbase=1, maj=13, min=0, rev=4, sub=0, bases=[0x3000] (8+4 = 12 bytes)
    //   DMU:  hw_id=271, inst=0, nbase=1, maj=3,  min=1, rev=4, sub=0, bases=[0x4000] (8+4 = 12 bytes)
    //   SDMA: hw_id=42,  inst=0, nbase=1, maj=6,  min=0, rev=3, sub=0, bases=[0x5000] (8+4 = 12 bytes)

    std::vector<uint8_t> buf(1024, 0);

    // Write binary_header
    *reinterpret_cast<uint32_t *>(&buf[0])  = kIpDiscoveryBinarySig;
    *reinterpret_cast<uint16_t *>(&buf[4])  = 2; // version_major
    *reinterpret_cast<uint16_t *>(&buf[6])  = 0; // version_minor
    *reinterpret_cast<uint16_t *>(&buf[10]) = 256; // binary_size
    *reinterpret_cast<uint16_t *>(&buf[12]) = 1; // num_tables

    // Write table_info[0]
    *reinterpret_cast<uint16_t *>(&buf[16]) = 0x20; // ipd offset
    *reinterpret_cast<uint16_t *>(&buf[20]) = 88;   // ipd size

    // Write ip_discovery_header
    *reinterpret_cast<uint32_t *>(&buf[0x20]) = kIpdsTableSig;
    *reinterpret_cast<uint16_t *>(&buf[0x24]) = 3;   // version
    *reinterpret_cast<uint16_t *>(&buf[0x26]) = 88;  // size
    *reinterpret_cast<uint32_t *>(&buf[0x28]) = 0;   // id
    *reinterpret_cast<uint16_t *>(&buf[0x2C]) = 1;   // num_dies (0x20 + 12)
    *reinterpret_cast<uint16_t *>(&buf[0x2E]) = 0;   // die_info[0].die_id (0x20 + 14)
    *reinterpret_cast<uint16_t *>(&buf[0x30]) = 0x80; // die_info[0].die_offset (0x20 + 16)

    // Write die_header
    *reinterpret_cast<uint16_t *>(&buf[0x80]) = 0; // die_id
    *reinterpret_cast<uint16_t *>(&buf[0x82]) = 4; // num_ips

    // IP entry 0: GC v11.0.3
    size_t p = 0x84;
    *reinterpret_cast<uint16_t *>(&buf[p + 0]) = static_cast<uint16_t>(HwId::Gc);
    buf[p + 2] = 0; // instance
    buf[p + 3] = 2; // num_bases
    buf[p + 4] = 11; // major
    buf[p + 5] = 0;  // minor
    buf[p + 6] = 3;  // rev
    buf[p + 7] = 0;
    *reinterpret_cast<uint32_t *>(&buf[p + 8])  = 0x00001000;
    *reinterpret_cast<uint32_t *>(&buf[p + 12]) = 0x00002000;
    p += 16;

    // IP entry 1: MP0 v13.0.4
    *reinterpret_cast<uint16_t *>(&buf[p + 0]) = static_cast<uint16_t>(HwId::Mp0);
    buf[p + 2] = 0;
    buf[p + 3] = 1;
    buf[p + 4] = 13;
    buf[p + 5] = 0;
    buf[p + 6] = 4;
    buf[p + 7] = 0;
    *reinterpret_cast<uint32_t *>(&buf[p + 8]) = 0x00003000;
    p += 12;

    // IP entry 2: DMU v3.1.4
    *reinterpret_cast<uint16_t *>(&buf[p + 0]) = static_cast<uint16_t>(HwId::Dmu);
    buf[p + 2] = 0;
    buf[p + 3] = 1;
    buf[p + 4] = 3;
    buf[p + 5] = 1;
    buf[p + 6] = 4;
    buf[p + 7] = 0;
    *reinterpret_cast<uint32_t *>(&buf[p + 8]) = 0x00004000;
    p += 12;

    // IP entry 3: SDMA0 v6.0.3
    *reinterpret_cast<uint16_t *>(&buf[p + 0]) = static_cast<uint16_t>(HwId::Sdma0);
    buf[p + 2] = 0;
    buf[p + 3] = 1;
    buf[p + 4] = 6;
    buf[p + 5] = 0;
    buf[p + 6] = 3;
    buf[p + 7] = 0;
    *reinterpret_cast<uint32_t *>(&buf[p + 8]) = 0x00005000;
    p += 12;

    // Compute checksum (16-bit sum of bytes [10 .. binary_size])
    uint16_t sum = 0;
    for (size_t i = 10; i < 256; ++i) {
        sum = static_cast<uint16_t>(sum + buf[i]);
    }
    *reinterpret_cast<uint16_t *>(&buf[8]) = sum;

    return buf;
}

static void test_ip_discovery_phoenix() {
    auto buf = create_synthetic_phoenix_ip_discovery();
    IpDiscoveryParser parser;

    assert(parser.init(buf.data(), buf.size()));
    assert(parser.is_valid());
    assert(parser.ip_count() == 4);
    assert(parser.is_phoenix_apu());

    IpEntry gc;
    assert(parser.find_ip(HwId::Gc, 0, gc));
    assert(gc.major == 11 && gc.minor == 0 && gc.revision == 3);
    assert(gc.num_bases == 2);
    assert(gc.bases[0] == 0x00001000);
    assert(gc.bases[1] == 0x00002000);

    // Register byte offset test
    uint32_t byte_off = 0;
    assert(parser.reg_byte_offset(HwId::Gc, 0, 0, 0x10, byte_off));
    assert(byte_off == (0x1000 + 0x10) * 4);

    assert(parser.reg_byte_offset(HwId::Gc, 0, 1, 0x20, byte_off));
    assert(byte_off == (0x2000 + 0x20) * 4);

    // Out of bounds segment
    assert(!parser.reg_byte_offset(HwId::Gc, 0, 5, 0x0, byte_off));

    // Absent IP
    IpEntry absent;
    assert(!parser.find_ip(HwId::Nbif, 0, absent));

    std::cout << "[PASS] test_ip_discovery_phoenix" << std::endl;
}

static void test_ip_discovery_fuzzer() {
    auto valid_buf = create_synthetic_phoenix_ip_discovery();

    uint32_t lcg = 0x98765432;
    auto rnd = [&]() -> uint32_t {
        lcg = lcg * 1664525u + 1013904223u;
        return lcg;
    };

    for (int iter = 0; iter < 1000; ++iter) {
        std::vector<uint8_t> corrupted = valid_buf;
        size_t mutations = 1 + (rnd() % 8);
        for (size_t m = 0; m < mutations; ++m) {
            size_t idx = rnd() % corrupted.size();
            corrupted[idx] ^= static_cast<uint8_t>(rnd() & 0xFF);
        }

        IpDiscoveryParser parser;
        bool ok = parser.init(corrupted.data(), corrupted.size());
        if (ok && parser.is_valid()) {
            IpEntry entry;
            for (uint16_t i = 0; i < parser.ip_count(); ++i) {
                parser.ip_at(i, entry);
            }
        }
    }
    std::cout << "[PASS] test_ip_discovery_fuzzer (1000 iterations)" << std::endl;
}

static void test_uma_rebar_full() {
    MemoryPlanInput in = {};
    in.is_apu           = true;
    in.bar0_len         = 4ull * kGiB;
    in.bar0_addr        = 0x800000000ull; // 32 GiB phys addr
    in.bar0_cfg_addr    = 0x800000000ull;
    in.rebar_present    = true;
    in.rebar_bytes      = 4ull * kGiB;
    in.vram_bytes       = 4ull * kGiB;
    in.tmr_reserve      = 64ull * kMiB;
    in.scanout_reserve  = 64ull * kMiB;
    in.console_off      = ~0ull;
    in.console_len      = 0;
    in.map_cap          = 4ull * kGiB;

    auto out = plan_memory_aperture(in);
    assert(out.ok);
    assert(out.status == RebarStatus::Ok);
    assert(out.is_full_rebar);
    assert(!out.has_hi_pool);
    assert(out.hi_pool_size == 0);

    // Usable VRAM = 4 GiB - 64 MiB (TMR) = 4032 MiB
    assert(out.vram_limit == (4ull * kGiB - 64ull * kMiB));
    assert(out.vis_pool_base == 64ull * kMiB);
    assert(out.vis_pool_size == out.vram_limit - out.vis_pool_base);

    // Verification points
    assert(out.aperture_pts[0] == 0);
    assert(out.aperture_pts[1] > 0);
    assert(out.aperture_pts[2] == out.vram_limit - 64ull * 1024);

    std::cout << "[PASS] test_uma_rebar_full (4 GiB UMA with Full ReBAR)" << std::endl;
}

static void test_uma_rebar_legacy() {
    MemoryPlanInput in = {};
    in.is_apu           = true;
    in.bar0_len         = 256ull * kMiB; // Legacy BAR0 (no ReBAR enabled)
    in.bar0_addr        = 0x80000000ull;
    in.bar0_cfg_addr    = 0x80000000ull;
    in.rebar_present    = false;
    in.rebar_bytes      = 0;
    in.vram_bytes       = 4ull * kGiB;
    in.tmr_reserve      = 64ull * kMiB;
    in.scanout_reserve  = 64ull * kMiB;
    in.console_off      = ~0ull;
    in.console_len      = 0;
    in.map_cap          = 4ull * kGiB;

    auto out = plan_memory_aperture(in);
    assert(out.ok);
    assert(!out.is_full_rebar);
    assert(out.has_hi_pool);

    // Visible window is 256 MiB
    assert(out.vram_limit == 256ull * kMiB);
    assert(out.vis_pool_size == 256ull * kMiB - 64ull * kMiB);

    // High pool covers remaining usable VRAM
    assert(out.hi_pool_base == 256ull * kMiB);
    assert(out.hi_pool_size == (4ull * kGiB - 64ull * kMiB - 256ull * kMiB));

    std::cout << "[PASS] test_uma_rebar_legacy (256 MiB BAR0 + 3776 MiB High Pool)" << std::endl;
}

static void test_uma_rebar_errors() {
    MemoryPlanInput in = {};
    in.is_apu           = true;
    in.bar0_len         = 4ull * kGiB;
    in.bar0_addr        = 0x800000000ull;
    in.bar0_cfg_addr    = 0x800000000ull;
    in.rebar_present    = true;
    in.rebar_bytes      = 4ull * kGiB;
    in.vram_bytes       = 4ull * kGiB;
    in.tmr_reserve      = 64ull * kMiB;
    in.scanout_reserve  = 64ull * kMiB;
    in.map_cap          = 4ull * kGiB;

    // Error 1: Len not pow2
    {
        auto err_in = in;
        err_in.bar0_len = 3ull * kGiB;
        auto out = plan_memory_aperture(err_in);
        assert(!out.ok && out.status == RebarStatus::LenNotPow2);
    }

    // Error 2: Misaligned address
    {
        auto err_in = in;
        err_in.bar0_addr = 0x800001000ull;
        auto out = plan_memory_aperture(err_in);
        assert(!out.ok && out.status == RebarStatus::AddrMisaligned);
    }

    // Error 3: ReBAR mismatch
    {
        auto err_in = in;
        err_in.rebar_bytes = 2ull * kGiB;
        auto out = plan_memory_aperture(err_in);
        assert(!out.ok && out.status == RebarStatus::RebarMismatch);
    }

    // Error 4: > 256 MiB without ReBAR capability
    {
        auto err_in = in;
        err_in.bar0_len = 512ull * kMiB;
        err_in.rebar_present = false;
        auto out = plan_memory_aperture(err_in);
        assert(!out.ok && out.status == RebarStatus::NoRebarCapAboveLegacy);
    }

    // Error 5: Console extends beyond limit
    {
        auto err_in = in;
        err_in.console_off = 4ull * kGiB;
        err_in.console_len = 16ull * kMiB;
        auto out = plan_memory_aperture(err_in);
        assert(!out.ok && out.status == RebarStatus::ConsoleBeyondLimit);
    }

    std::cout << "[PASS] test_uma_rebar_errors (all rejection invariants enforced)" << std::endl;
}

int main() {
    std::cout << "=== Running RDNA3 IP Discovery & UMA ReBAR Planner Tests ===" << std::endl;
    test_ip_discovery_phoenix();
    test_ip_discovery_fuzzer();
    test_uma_rebar_full();
    test_uma_rebar_legacy();
    test_uma_rebar_errors();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
