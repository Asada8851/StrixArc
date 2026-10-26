//
//  rdna3_ip_discovery.cpp — Freestanding IP discovery table parser.
//  Zero heap allocations, strict bounds checking.
//
#include "rdna3_ip_discovery.h"

namespace rdna3 {

bool IpDiscoveryParser::read_u8(size_t off, uint8_t &v) const {
    if (off >= size_) return false;
    v = data_[off];
    return true;
}

bool IpDiscoveryParser::read_u16(size_t off, uint16_t &v) const {
    if (off + 2 > size_) return false;
    v = static_cast<uint16_t>(data_[off] | (static_cast<uint16_t>(data_[off + 1]) << 8));
    return true;
}

bool IpDiscoveryParser::read_u32(size_t off, uint32_t &v) const {
    if (off + 4 > size_) return false;
    v = static_cast<uint32_t>(data_[off]) |
        (static_cast<uint32_t>(data_[off + 1]) << 8) |
        (static_cast<uint32_t>(data_[off + 2]) << 16) |
        (static_cast<uint32_t>(data_[off + 3]) << 24);
    return true;
}

bool IpDiscoveryParser::validate_binary(size_t offset) {
    uint16_t vmajor, checksum, bin_size;
    if (!read_u16(offset + 4, vmajor) ||
        !read_u16(offset + 8, checksum) ||
        !read_u16(offset + 10, bin_size)) {
        return false;
    }

    if (bin_size < 16 || offset + bin_size > size_) {
        return false;
    }

    // Checksum: 16-bit sum of all bytes after the checksum field
    uint16_t sum = 0;
    for (size_t i = offset + 10; i < offset + bin_size; ++i) {
        sum = static_cast<uint16_t>(sum + data_[i]);
    }
    if (sum != checksum) {
        return false;
    }

    // IP_DISCOVERY table is table_list[0]
    size_t table_list = offset + (vmajor >= 2 ? 16 : 12);
    uint16_t ipd_offset, ipd_size;
    if (!read_u16(table_list, ipd_offset) ||
        !read_u16(table_list + 4, ipd_size) ||
        ipd_offset == 0 || ipd_size < 80) {
        return false;
    }

    size_t ipd = offset + ipd_offset;
    uint32_t sig;
    uint16_t num_dies;
    if (!read_u32(ipd, sig) || sig != kIpdsTableSig ||
        !read_u16(ipd + 12, num_dies) || num_dies == 0) {
        return false;
    }

    // Die info 0 sits at offset ipd + 16 (u16 die_id at +14, u16 die_offset at +16)
    uint16_t die_offset;
    if (!read_u16(ipd + 16, die_offset) || die_offset == 0) {
        return false;
    }

    size_t die = offset + die_offset;
    uint16_t nips;
    if (!read_u16(die + 2, nips) || nips == 0 || nips > kMaxIpEntries) {
        return false;
    }

    bin_base_ = offset;
    die_base_ = die;
    num_ips_  = nips;
    return true;
}

bool IpDiscoveryParser::init(const uint8_t *buffer, size_t buffer_size) {
    data_ = buffer;
    size_ = buffer_size;
    valid_ = false;
    num_ips_ = 0;

    if (!data_ || size_ < 512) {
        return false;
    }

    // Scan for kIpDiscoveryBinarySig (0x28211407) on 4-byte boundaries
    for (size_t off = 0; off + 512 <= size_; off += 4) {
        uint32_t sig;
        if (read_u32(off, sig) && sig == kIpDiscoveryBinarySig && validate_binary(off)) {
            valid_ = true;
            return true;
        }
    }

    return false;
}

bool IpDiscoveryParser::ip_at(uint16_t index, IpEntry &out) const {
    if (!valid_ || index >= num_ips_) return false;

    // IP entry format:
    // u16 hw_id, u8 instance, u8 num_bases, u8 major, u8 minor, u8 revision, u8 sub_type/variant, u32 bases[]
    size_t p = die_base_ + 4;
    for (uint16_t i = 0; ; ++i) {
        uint16_t hw_id;
        uint8_t inst, nbase, major, minor, rev;
        if (!read_u16(p, hw_id) ||
            !read_u8(p + 2, inst) ||
            !read_u8(p + 3, nbase) ||
            !read_u8(p + 4, major) ||
            !read_u8(p + 5, minor) ||
            !read_u8(p + 6, rev)) {
            return false;
        }

        if (i == index) {
            out.hw_id     = hw_id;
            out.instance  = inst;
            out.major     = major;
            out.minor     = minor;
            out.revision  = rev;
            out.num_bases = (nbase < kMaxIpBases) ? nbase : static_cast<uint8_t>(kMaxIpBases);
            for (uint8_t b = 0; b < out.num_bases; ++b) {
                if (!read_u32(p + 8 + 4u * b, out.bases[b])) {
                    return false;
                }
            }
            return true;
        }

        // Advance to next IP entry: 8 header bytes + 4 * nbase
        p += 8 + 4u * nbase;
        if (p >= size_) return false;
    }
}

bool IpDiscoveryParser::find_ip(uint16_t hw_id, uint8_t instance, IpEntry &out) const {
    for (uint16_t i = 0; i < num_ips_; ++i) {
        if (!ip_at(i, out)) return false;
        if (out.hw_id == hw_id && out.instance == instance) {
            return true;
        }
    }
    return false;
}

bool IpDiscoveryParser::find_ip(HwId hw_id, uint8_t instance, IpEntry &out) const {
    return find_ip(static_cast<uint16_t>(hw_id), instance, out);
}

bool IpDiscoveryParser::reg_byte_offset(HwId hw_id, uint8_t instance, uint8_t segment,
                                        uint32_t reg_dword_offset, uint32_t &byte_offset) const {
    IpEntry ip;
    if (!find_ip(hw_id, instance, ip) || segment >= ip.num_bases) {
        return false;
    }
    byte_offset = (ip.bases[segment] + reg_dword_offset) * 4;
    return true;
}

bool IpDiscoveryParser::is_phoenix_apu() const {
    if (!valid_) return false;

    IpEntry gc, psp, dcn, sdma;
    if (!find_ip(HwId::Gc, 0, gc) ||
        !find_ip(HwId::Mp0, 0, psp) ||
        !find_ip(HwId::Dmu, 0, dcn) ||
        !find_ip(HwId::Sdma0, 0, sdma)) {
        return false;
    }

    // Verify Phoenix IP generations:
    // GC: 11.0.x (GFX1103)
    // PSP: 13.0.x
    // DCN: 3.1.x (DCN 3.1.4)
    // SDMA: 6.0.x
    bool gc_match   = (gc.major == 11 && gc.minor == 0);
    bool psp_match  = (psp.major == 13 && psp.minor == 0);
    bool dcn_match  = (dcn.major == 3 && dcn.minor == 1);
    bool sdma_match = (sdma.major == 6 && sdma.minor == 0);

    return gc_match && psp_match && dcn_match && sdma_match;
}

} // namespace rdna3
