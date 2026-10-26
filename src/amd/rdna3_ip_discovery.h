//
//  rdna3_ip_discovery.h — On-die IP discovery table parser for RDNA 3 / Phoenix APU.
//  Freestanding C++17, zero allocations, bounds-checked.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace rdna3 {

// Hardware IP IDs from Linux soc15_hw_ip.h / amdgpu discovery:
enum class HwId : uint16_t {
    Mp1     = 1,    // SMU / Power management
    Gc      = 11,   // Graphics & Compute (GFX11 / RDNA 3)
    Mmhub   = 34,   // Memory management hub
    Osssys  = 40,   // IH / OS system
    Hdp     = 41,   // Host Data Path
    Sdma0   = 42,   // System DMA 0
    Nbif    = 108,  // NBIO / Bus interface
    Umc     = 150,  // Unified Memory Controller
    Mp0     = 255,  // PSP / Security processor
    Dmu     = 271,  // Display micro-unit (DCN 3.1.4)
};

constexpr uint32_t kIpDiscoveryBinarySig = 0x28211407;
constexpr uint32_t kIpdsTableSig         = 0x53445049; // "IPDS" LE
constexpr size_t   kMaxIpBases           = 16;
constexpr size_t   kMaxIpEntries         = 128;

struct IpEntry {
    uint16_t hw_id;
    uint8_t  instance;
    uint8_t  major;
    uint8_t  minor;
    uint8_t  revision;
    uint8_t  num_bases;
    uint32_t bases[kMaxIpBases]; // dword register base addresses
};

class IpDiscoveryParser {
public:
    IpDiscoveryParser() = default;

    // Initializes parser from memory buffer containing the IP discovery binary.
    // Validates checksum, table signature, and parses die 0.
    bool init(const uint8_t *buffer, size_t buffer_size);

    bool is_valid() const { return valid_; }
    uint16_t ip_count() const { return num_ips_; }

    // Retrieves IP entry at index (0 .. ip_count() - 1).
    bool ip_at(uint16_t index, IpEntry &out) const;

    // Finds specific IP block instance (e.g. HwId::Gc, instance 0).
    bool find_ip(HwId hw_id, uint8_t instance, IpEntry &out) const;
    bool find_ip(uint16_t hw_id, uint8_t instance, IpEntry &out) const;

    // Computes absolute MMIO byte offset for a register given its segment and dword offset.
    bool reg_byte_offset(HwId hw_id, uint8_t instance, uint8_t segment,
                         uint32_t reg_dword_offset, uint32_t &byte_offset) const;

    // Validates that discovered IPs match Phoenix APU (GFX 11.x, PSP 13.x, SDMA 6.x, DCN 3.1.x).
    bool is_phoenix_apu() const;

private:
    const uint8_t *data_ { nullptr };
    size_t size_ { 0 };
    size_t bin_base_ { 0 };
    size_t die_base_ { 0 };
    uint16_t num_ips_ { 0 };
    bool valid_ { false };

    bool read_u8 (size_t off, uint8_t  &v) const;
    bool read_u16(size_t off, uint16_t &v) const;
    bool read_u32(size_t off, uint32_t &v) const;
    bool validate_binary(size_t offset);
};

} // namespace rdna3
