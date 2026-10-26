//
//  test_r3n_abi.cpp — Unit tests for R3N UserClient ABI and Mesa RADV winsys adapter.
//  Builds cleanly under Clang with -fsanitize=address,undefined.
//
#include "../src/uapi/r3n_user_client.h"
#include "../src/winsys/radv_r3n_winsys.h"
#include <iostream>
#include <cassert>

using namespace rdna3;

static void test_r3n_client_handshake() {
    DeviceContext ctx = {};
    ctx.vram_size_bytes = 4ull * kGiB;
    ctx.vram_limit      = 4032ull * kMiB;

    R3nUserClient client(&ctx);

    // Call query_info before hello should fail
    r3n_info info = {};
    assert(!client.query_info(info));

    // Hello with wrong ABI version should fail
    uint32_t out[4] = {};
    assert(!client.hello(99, 0, out));

    // Valid Hello
    assert(client.hello(R3N_ABI_VERSION, 0, out));
    assert((out[0] & 0xFFFF) == R3N_ABI_VERSION);
    assert(out[2] == R3N_MAX_IBS);

    // Valid query_info
    assert(client.query_info(info));
    assert(info.chip_id == 0x1900);
    assert(info.chip_rev == 0xBA);
    assert(info.ip_version_major == 11);
    assert(info.ip_version_rev == 3);
    assert(info.cu_count == 12);
    assert(info.vram_size_bytes == 4ull * kGiB);
    assert(info.display_width == 2560);
    assert(info.display_height == 1600);
    assert(info.display_refresh_hz == 120);

    std::cout << "[PASS] test_r3n_client_handshake (Hello & QueryInfo for GFX1103)" << std::endl;
}

static void test_r3n_bo_and_vm() {
    DeviceContext ctx = {};
    R3nUserClient client(&ctx);
    uint32_t out[4] = {};
    assert(client.hello(R3N_ABI_VERSION, 0, out));

    // Create 10000 bytes BO -> aligned to 12288 bytes (4 KiB multiple)
    r3n_gem_create_in create_in = {};
    create_in.size_bytes = 10000;
    create_in.domain     = R3N_GEM_DOMAIN_VRAM;
    uint32_t handle = 0;
    uint64_t allocated_size = 0;
    assert(client.bo_create(create_in, handle, allocated_size));
    assert(handle > 0);
    assert(allocated_size == 12288);

    // Map into GPU VA: unaligned VA fails
    r3n_gem_va va_in = {};
    va_in.handle   = handle;
    va_in.op       = R3N_VA_OP_MAP;
    va_in.gpu_va   = 0x10000001; // unaligned!
    va_in.map_size = allocated_size;
    assert(!client.gem_va(va_in));

    // Aligned VA succeeds
    va_in.gpu_va   = 0x10000000;
    assert(client.gem_va(va_in));

    // Second map on same BO fails
    assert(!client.gem_va(va_in));

    // Unmap
    va_in.op = R3N_VA_OP_UNMAP;
    assert(client.gem_va(va_in));

    // Free BO
    assert(client.bo_free(handle));
    // Free already freed BO fails
    assert(!client.bo_free(handle));

    std::cout << "[PASS] test_r3n_bo_and_vm (BO alloc, 4 KiB align, GPU VA map/unmap)" << std::endl;
}

static void test_r3n_cs_and_scanout() {
    DeviceContext ctx = {};
    R3nUserClient client(&ctx);
    uint32_t out[4] = {};
    assert(client.hello(R3N_ABI_VERSION, 0, out));

    // Create context
    uint32_t ctx_id = 0;
    assert(client.ctx_create(ctx_id));
    assert(ctx_id > 0);

    // Submit CS with valid IB
    r3n_cs_in cs = {};
    cs.ctx_id     = ctx_id;
    cs.num_ibs    = 1;
    cs.ibs[0].gpu_va      = 0x20000000;
    cs.ibs[0].size_dwords = 256;
    cs.ibs[0].ip_type     = 0; // GFX

    uint64_t seqno = 0;
    assert(client.submit_cs(cs, seqno));
    assert(seqno > 0);

    // Wait on fence
    bool retired = false;
    assert(client.wait_seq(seqno, 1000000, retired));
    assert(retired);

    // Scanout workflow
    assert(client.scan_acquire());

    // Allocate 2560x1600 scanout buffer
    r3n_gem_create_in fb_in = {};
    fb_in.size_bytes = 2560 * 1600 * 4;
    fb_in.domain     = R3N_GEM_DOMAIN_VRAM;
    uint32_t fb_handle = 0;
    uint64_t fb_size = 0;
    assert(client.bo_create(fb_in, fb_handle, fb_size));

    // Register scanout
    r3n_scan_reg reg_in = {};
    reg_in.bo_handle    = fb_handle;
    reg_in.width        = 2560;
    reg_in.height       = 1600;
    reg_in.stride_bytes = 2560 * 4;
    reg_in.format       = 4; // ARGB8888
    uint32_t slot_id = 0;
    assert(client.scan_register(reg_in, slot_id));
    assert(slot_id > 0);

    // Present frame
    r3n_scan_present pres_in = {};
    pres_in.slot_id = slot_id;
    uint64_t pres_id = 0;
    assert(client.scan_present(pres_in, pres_id));
    assert(pres_id == 1);

    assert(client.scan_release());
    assert(client.ctx_destroy(ctx_id));

    std::cout << "[PASS] test_r3n_cs_and_scanout (PM4 CS submission, fence, 2560x1600 scanout present)" << std::endl;
}

static void test_radv_winsys_adapter() {
    DeviceContext ctx = {};
    ctx.vram_size_bytes = 4ull * kGiB;
    ctx.vram_limit      = 4032ull * kMiB;

    R3nUserClient client(&ctx);
    RadvR3nWinsys winsys(&client);

    assert(winsys.init());
    const auto &gpu = winsys.get_gpu_info();
    assert(gpu.chip_class == 11); // GFX11
    assert(gpu.num_cu == 12);
    assert(gpu.has_rebar);
    assert(!gpu.has_dedicated_vram);

    // Create context and allocate BO
    uint32_t ctx_id = 0;
    assert(client.ctx_create(ctx_id));

    auto *bo = winsys.alloc_bo(65536, R3N_GEM_DOMAIN_VRAM, 0x40000000);
    assert(bo != nullptr);
    assert(bo->va == 0x40000000);
    assert(bo->size >= 65536);

    // Submit IB through winsys
    uint64_t seqno = winsys.submit_ib(ctx_id, bo->va, 1024, 0);
    assert(seqno > 0);
    assert(winsys.wait_fence(seqno, 5000000));

    winsys.free_bo(bo);
    assert(client.ctx_destroy(ctx_id));

    std::cout << "[PASS] test_radv_winsys_adapter (Mesa RADV winsys initialization, BO alloc, and submit)" << std::endl;
}

int main() {
    std::cout << "=== Running R3N UserClient ABI & Mesa RADV Winsys Tests ===" << std::endl;
    test_r3n_client_handshake();
    test_r3n_bo_and_vm();
    test_r3n_cs_and_scanout();
    test_radv_winsys_adapter();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
