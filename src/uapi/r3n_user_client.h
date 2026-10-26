//
//  r3n_user_client.h — R3N UserClient session manager for RDNA 3 / Phoenix.
//
#pragma once
#include "r3n_native_abi.h"
#include "../amd/rdna3_ip_blocks.h"
#include <map>
#include <vector>

namespace rdna3 {

struct BufferObject {
    uint32_t handle;
    uint64_t size_bytes;
    uint32_t domain;
    uint64_t gpu_va;
    bool is_mapped;
    bool is_scanout;
};

class R3nUserClient {
public:
    explicit R3nUserClient(DeviceContext *ctx);
    ~R3nUserClient();

    // Transport methods matching IOConnectCallMethod dispatch
    bool hello(uint32_t client_abi, uint32_t flags, uint32_t out[4]);
    bool query_info(r3n_info &info);

    bool bo_create(const r3n_gem_create_in &in, uint32_t &handle, uint64_t &allocated_size);
    bool bo_free(uint32_t handle);
    bool gem_va(const r3n_gem_va &in);

    bool ctx_create(uint32_t &ctx_id);
    bool ctx_destroy(uint32_t ctx_id);

    bool submit_cs(const r3n_cs_in &in, uint64_t &seqno);
    bool wait_seq(uint64_t seqno, uint64_t timeout_ns, bool &retired);

    // Scanout methods
    bool scan_acquire();
    bool scan_register(const r3n_scan_reg &in, uint32_t &slot_id);
    bool scan_present(const r3n_scan_present &in, uint64_t &present_id);
    bool scan_release();

private:
    DeviceContext *ctx_ { nullptr };
    bool handshaked_ { false };
    bool scanout_acquired_ { false };

    uint32_t next_handle_ { 1 };
    uint32_t next_ctx_id_ { 1 };
    uint64_t last_seqno_  { 0 };
    uint64_t last_retired_seqno_ { 0 };
    uint64_t present_count_ { 0 };

    std::map<uint32_t, BufferObject> bos_;
    std::vector<uint32_t> contexts_;
    std::map<uint32_t, uint32_t> scanout_slots_; // slot_id -> bo_handle
};

} // namespace rdna3
