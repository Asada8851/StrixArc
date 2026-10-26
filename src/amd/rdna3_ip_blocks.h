//
//  rdna3_ip_blocks.h — IP Block state machines and initialization for Phoenix APU (RDNA 3 / GFX1103).
//  Pure C++17 freestanding, mockable MMIO interface for host verification under ASan/UBSan.
//
#pragma once
#include "rdna3_ip_discovery.h"
#include "rdna3_rebar_uma.h"
#include "rdna3_ucode_extract.h"
#include <stdint.h>
#include <stddef.h>

namespace rdna3 {

// MMIO access interface
class IMmioBus {
public:
    virtual ~IMmioBus() = default;
    virtual uint32_t read32(uint32_t dword_offset) = 0;
    virtual void write32(uint32_t dword_offset, uint32_t value) = 0;
};

// Memory Context for Phoenix APU
struct DeviceContext {
    IMmioBus *mmio { nullptr };
    const IpDiscoveryParser *discovery { nullptr };

    // PCIe BAR physical geometry
    uint64_t bar0_phys { 0x0000007ff0000000ull }; // 256 MiB legacy or 4 GiB ReBAR
    uint64_t bar0_size { 256ull * kMiB };
    uint64_t bar2_phys { 0x00000000fc600000ull }; // Doorbell BAR (2 MiB)
    uint64_t bar2_size { 2ull * kMiB };
    uint64_t bar5_phys { 0x00000000fcb00000ull }; // MMIO BAR (512 KiB)
    uint64_t bar5_size { 512ull * 1024 };

    // UMA VRAM geometry
    uint64_t vram_size_bytes { 4ull * kGiB };
    uint64_t vram_mc_base    { 0x0000008000000000ull }; // FB location base in MC space
    uint64_t vram_limit      { 0 };

    // IP State flags
    bool gmc_ready   { false };
    bool psp_online  { false };
    bool smu_online  { false };
    bool sdma_ready  { false };
    bool gfx_ready   { false };

    // TMR region allocated in VRAM
    uint64_t tmr_mc_addr { 0 };
    uint64_t tmr_size    { 64ull * kMiB };
};

// GMC v11.0 State Machine (Graphics Memory Controller / MMHUB)
class Gmc11Block {
public:
    static bool init(DeviceContext &ctx, const MemoryPlanOutput &mem_plan);
    static bool is_ready(const DeviceContext &ctx) { return ctx.gmc_ready; }
};

// PSP v13.0 State Machine (Platform Security Processor)
class Psp13Block {
public:
    enum class BootloaderCmd : uint32_t {
        LoadSysDrv    = 0x10000,
        LoadSosDrv    = 0x20000,
        LoadKeyDb     = 0x80000,
        LoadSocDrv    = 0xB0000,
        LoadIntfDrv   = 0xD0000,
        LoadRasDrv    = 0xE0000,
    };

    static bool init(DeviceContext &ctx);
    static bool load_tmr(DeviceContext &ctx, uint64_t tmr_mc_addr, uint32_t tmr_size);
    static bool load_ip_firmware(DeviceContext &ctx, PSPGfxFwType fw_type, uint64_t fw_mc_addr, uint32_t fw_size);
    static bool is_online(const DeviceContext &ctx) { return ctx.psp_online; }
};

// SMU v13.0 State Machine (System Management Unit / PMFW)
class Smu13Block {
public:
    static bool init(DeviceContext &ctx);
    static bool send_msg(DeviceContext &ctx, uint32_t msg_id, uint32_t arg, uint32_t *resp_val = nullptr);
    static bool is_online(const DeviceContext &ctx) { return ctx.smu_online; }
};

// SDMA v6.0 State Machine (System DMA engine on Phoenix)
class Sdma6Block {
public:
    static bool init(DeviceContext &ctx, uint64_t ring_mc_addr, uint32_t ring_size_bytes);
    static bool is_ready(const DeviceContext &ctx) { return ctx.sdma_ready; }
};

// GFX v11.0 State Machine (CP RS64, MEC, RLC)
class Gfx11Block {
public:
    static bool init(DeviceContext &ctx, uint64_t ring_mc_addr, uint32_t ring_size_bytes);
    static bool enable_rlc_autoload(DeviceContext &ctx);
    static bool is_ready(const DeviceContext &ctx) { return ctx.gfx_ready; }
};

// Top-level device orchestrator
class Rdna3Device {
public:
    static bool bringup(DeviceContext &ctx, const MemoryPlanOutput &mem_plan);
};

} // namespace rdna3
