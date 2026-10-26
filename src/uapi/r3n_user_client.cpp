//
//  r3n_user_client.cpp — Implementation of R3N UserClient session manager.
//
#include "r3n_user_client.h"
#include <string.h>
#include <algorithm>

namespace rdna3 {

R3nUserClient::R3nUserClient(DeviceContext *ctx)
    : ctx_(ctx) {}

R3nUserClient::~R3nUserClient() {
    if (scanout_acquired_) {
        scan_release();
    }
    bos_.clear();
    contexts_.clear();
}

bool R3nUserClient::hello(uint32_t client_abi, uint32_t flags, uint32_t out[4]) {
    (void)flags;
    if (client_abi != R3N_ABI_VERSION || !out) return false;

    out[0] = R3N_ABI_VERSION | (R3N_ABI_MINOR << 16);
    out[1] = 0x00010000; // Driver build 1.0
    out[2] = R3N_MAX_IBS;
    out[3] = 1;          // VMID 1

    handshaked_ = true;
    return true;
}

bool R3nUserClient::query_info(r3n_info &info) {
    if (!handshaked_ || !ctx_) return false;
    memset(&info, 0, sizeof(info));

    info.chip_id            = 0x1900;
    info.chip_rev           = 0xBA;
    info.family             = 143; // AMD_FAMILY_GC_11_0_3
    info.ip_version_major   = 11;
    info.ip_version_minor   = 0;
    info.ip_version_rev     = 3;
    info.vram_size_bytes    = ctx_->vram_size_bytes;
    info.vram_visible_bytes = (ctx_->vram_limit > 0) ? ctx_->vram_limit : 256ull * kMiB;
    info.gtt_size_bytes     = 8ull * kGiB;
    info.cu_count           = 12; // Radeon 780M
    info.max_engine_clock_mhz = 2700;
    info.max_memory_clock_mhz = 3200;
    info.display_width      = 2560;
    info.display_height     = 1600;
    info.display_refresh_hz = 120;

    return true;
}

bool R3nUserClient::bo_create(const r3n_gem_create_in &in, uint32_t &handle, uint64_t &allocated_size) {
    if (!handshaked_ || in.size_bytes == 0) return false;

    // Align size to 4 KiB
    uint64_t align = in.alignment_bytes ? in.alignment_bytes : 4096;
    uint64_t size  = (in.size_bytes + align - 1) & ~(align - 1);

    handle = next_handle_++;
    BufferObject bo = {};
    bo.handle      = handle;
    bo.size_bytes  = size;
    bo.domain      = in.domain;
    bo.gpu_va      = 0;
    bo.is_mapped   = false;
    bo.is_scanout  = false;

    bos_[handle] = bo;
    allocated_size = size;
    return true;
}

bool R3nUserClient::bo_free(uint32_t handle) {
    if (!handshaked_ || handle == 0) return false;
    auto it = bos_.find(handle);
    if (it == bos_.end()) return false;
    bos_.erase(it);
    return true;
}

bool R3nUserClient::gem_va(const r3n_gem_va &in) {
    if (!handshaked_) return false;
    auto it = bos_.find(in.handle);
    if (it == bos_.end()) return false;

    if (in.op == R3N_VA_OP_MAP) {
        if (it->second.is_mapped) return false; // already mapped
        if ((in.gpu_va & 0xFFF) != 0) return false; // must be 4 KiB aligned
        it->second.gpu_va    = in.gpu_va;
        it->second.is_mapped = true;
        return true;
    } else if (in.op == R3N_VA_OP_UNMAP) {
        if (!it->second.is_mapped) return false;
        it->second.gpu_va    = 0;
        it->second.is_mapped = false;
        return true;
    }

    return false;
}

bool R3nUserClient::ctx_create(uint32_t &ctx_id) {
    if (!handshaked_) return false;
    ctx_id = next_ctx_id_++;
    contexts_.push_back(ctx_id);
    return true;
}

bool R3nUserClient::ctx_destroy(uint32_t ctx_id) {
    if (!handshaked_) return false;
    auto it = std::find(contexts_.begin(), contexts_.end(), ctx_id);
    if (it == contexts_.end()) return false;
    contexts_.erase(it);
    return true;
}

bool R3nUserClient::submit_cs(const r3n_cs_in &in, uint64_t &seqno) {
    if (!handshaked_ || in.num_ibs == 0 || in.num_ibs > R3N_MAX_IBS) return false;

    // Verify context exists
    auto it = std::find(contexts_.begin(), contexts_.end(), in.ctx_id);
    if (it == contexts_.end()) return false;

    // Validate IBs
    for (uint32_t i = 0; i < in.num_ibs; ++i) {
        if (in.ibs[i].size_dwords == 0 || (in.ibs[i].gpu_va & 3) != 0) {
            return false;
        }
    }

    // Emits new sequence number
    seqno = ++last_seqno_;
    // In mock/simulated pipeline, work completes
    last_retired_seqno_ = seqno;
    return true;
}

bool R3nUserClient::wait_seq(uint64_t seqno, uint64_t timeout_ns, bool &retired) {
    (void)timeout_ns;
    if (!handshaked_) return false;
    retired = (seqno <= last_retired_seqno_);
    return true;
}

bool R3nUserClient::scan_acquire() {
    if (!handshaked_ || scanout_acquired_) return false;
    scanout_acquired_ = true;
    return true;
}

bool R3nUserClient::scan_register(const r3n_scan_reg &in, uint32_t &slot_id) {
    if (!handshaked_ || !scanout_acquired_) return false;
    auto it = bos_.find(in.bo_handle);
    if (it == bos_.end()) return false;

    // Validate 2560x1600 geometry
    if (in.width != 2560 || in.height != 1600 || in.stride_bytes < 2560 * 4) {
        return false;
    }

    slot_id = static_cast<uint32_t>(scanout_slots_.size() + 1);
    scanout_slots_[slot_id] = in.bo_handle;
    it->second.is_scanout = true;
    return true;
}

bool R3nUserClient::scan_present(const r3n_scan_present &in, uint64_t &present_id) {
    (void)in;
    if (!handshaked_ || !scanout_acquired_) return false;
    auto it = scanout_slots_.find(in.slot_id);
    if (it == scanout_slots_.end()) return false;

    present_id = ++present_count_;
    return true;
}

bool R3nUserClient::scan_release() {
    if (!scanout_acquired_) return false;
    scanout_acquired_ = false;
    scanout_slots_.clear();
    return true;
}

} // namespace rdna3
