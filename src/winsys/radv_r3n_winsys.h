//
//  radv_r3n_winsys.h — Mesa RADV Vulkan winsys adapter for R3N UserClient.
//  Bridges Mesa RADV Vulkan driver interface to the freestanding R3N transport.
//
#pragma once
#include "../uapi/r3n_user_client.h"

namespace rdna3 {

struct RadvGpuInfo {
    uint32_t pci_id;
    uint32_t chip_class;       // GFX11 (11)
    uint32_t family;           // CHIP_PHOENIX
    uint32_t num_cu;           // 12
    uint32_t max_waves_per_cu; // 32
    uint64_t vram_size_bytes;
    uint64_t gtt_size_bytes;
    bool     has_dedicated_vram; // false for APU
    bool     has_rebar;
};

struct RadvBo {
    uint32_t handle;
    uint64_t size;
    uint64_t va;
    void    *cpu_ptr;
};

class RadvR3nWinsys {
public:
    explicit RadvR3nWinsys(R3nUserClient *client);

    bool init();
    const RadvGpuInfo &get_gpu_info() const { return gpu_info_; }

    RadvBo *alloc_bo(uint64_t size, uint32_t domain, uint64_t gpu_va);
    void free_bo(RadvBo *bo);

    uint64_t submit_ib(uint32_t ctx_id, uint64_t ib_gpu_va, uint32_t size_dwords, uint32_t ip_type);
    bool wait_fence(uint64_t fence_seqno, uint64_t timeout_ns);

private:
    R3nUserClient *client_ { nullptr };
    RadvGpuInfo    gpu_info_ = {};
    bool           initialized_ { false };
};

} // namespace rdna3
