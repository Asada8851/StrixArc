//
//  radv_r3n_winsys.cpp — Implementation of Mesa RADV Vulkan winsys adapter.
//
#include "radv_r3n_winsys.h"
#include <string.h>

namespace rdna3 {

RadvR3nWinsys::RadvR3nWinsys(R3nUserClient *client)
    : client_(client) {}

bool RadvR3nWinsys::init() {
    if (!client_) return false;

    uint32_t out[4] = {};
    if (!client_->hello(R3N_ABI_VERSION, 0, out)) return false;

    r3n_info info = {};
    if (!client_->query_info(info)) return false;

    gpu_info_.pci_id             = (info.chip_id << 16) | info.chip_rev;
    gpu_info_.chip_class         = info.ip_version_major; // 11 (GFX11)
    gpu_info_.family             = info.family;           // CHIP_PHOENIX
    gpu_info_.num_cu             = info.cu_count;         // 12
    gpu_info_.max_waves_per_cu   = 32;
    gpu_info_.vram_size_bytes    = info.vram_size_bytes;
    gpu_info_.gtt_size_bytes     = info.gtt_size_bytes;
    gpu_info_.has_dedicated_vram = false;                 // APU
    gpu_info_.has_rebar          = (info.vram_visible_bytes >= info.vram_size_bytes - 64ull * kMiB);

    initialized_ = true;
    return true;
}

RadvBo *RadvR3nWinsys::alloc_bo(uint64_t size, uint32_t domain, uint64_t gpu_va) {
    if (!initialized_ || size == 0) return nullptr;

    r3n_gem_create_in in = {};
    in.size_bytes      = size;
    in.alignment_bytes = 4096;
    in.domain          = domain;

    uint32_t handle = 0;
    uint64_t allocated_size = 0;
    if (!client_->bo_create(in, handle, allocated_size)) return nullptr;

    // Map into GPU VA space
    if (gpu_va != 0) {
        r3n_gem_va va_in = {};
        va_in.handle    = handle;
        va_in.op        = R3N_VA_OP_MAP;
        va_in.flags     = R3N_VM_PAGE_READABLE | R3N_VM_PAGE_WRITEABLE;
        va_in.gpu_va    = gpu_va;
        va_in.map_size  = allocated_size;
        if (!client_->gem_va(va_in)) {
            client_->bo_free(handle);
            return nullptr;
        }
    }

    auto *bo = new RadvBo();
    bo->handle  = handle;
    bo->size    = allocated_size;
    bo->va      = gpu_va;
    bo->cpu_ptr = nullptr;
    return bo;
}

void RadvR3nWinsys::free_bo(RadvBo *bo) {
    if (!bo || !initialized_) return;

    if (bo->va != 0) {
        r3n_gem_va va_in = {};
        va_in.handle = bo->handle;
        va_in.op     = R3N_VA_OP_UNMAP;
        client_->gem_va(va_in);
    }

    client_->bo_free(bo->handle);
    delete bo;
}

uint64_t RadvR3nWinsys::submit_ib(uint32_t ctx_id, uint64_t ib_gpu_va, uint32_t size_dwords, uint32_t ip_type) {
    if (!initialized_) return 0;

    r3n_cs_in cs = {};
    cs.ctx_id     = ctx_id;
    cs.num_ibs    = 1;
    cs.ibs[0].gpu_va      = ib_gpu_va;
    cs.ibs[0].size_dwords = size_dwords;
    cs.ibs[0].ip_type     = ip_type;

    uint64_t seqno = 0;
    if (!client_->submit_cs(cs, seqno)) return 0;
    return seqno;
}

bool RadvR3nWinsys::wait_fence(uint64_t fence_seqno, uint64_t timeout_ns) {
    if (!initialized_) return false;
    bool retired = false;
    if (!client_->wait_seq(fence_seqno, timeout_ns, retired)) return false;
    return retired;
}

} // namespace rdna3
