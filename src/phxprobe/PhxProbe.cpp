//
//  PhxProbe.cpp — read-only probe for the AMD Phoenix / Hawk Point iGPU (1002:1900).
//
//  Maps the MMIO BAR, reads the registers Linux amdgpu uses before it knows the IP layout
//  (amdgpu_discovery.c: RCC_CONFIG_MEMSIZE, MP0_SMN_C2PMSG_33, DRIVER_SCRATCH_0..2) and copies
//  the IP discovery table out of the UMA carve-out through the MM_INDEX/MM_DATA window.
//  Everything is published as IORegistry properties; nothing on the GPU is configured.
//  It uses its own IOMatchCategory, so the firmware framebuffer keeps the display.
//  Boot-arg -phxprobeoff disables it.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>

class PhxProbe : public IOService {
    OSDeclareDefaultStructors(PhxProbe)
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PhxProbe, IOService)

// Dword register indices, from Linux drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c.
static constexpr uint32_t mmMM_INDEX           = 0x0;
static constexpr uint32_t mmMM_DATA            = 0x1;
static constexpr uint32_t mmMM_INDEX_HI        = 0x6;
static constexpr uint32_t mmDRIVER_SCRATCH_0   = 0x94;
static constexpr uint32_t mmDRIVER_SCRATCH_1   = 0x95;
static constexpr uint32_t mmDRIVER_SCRATCH_2   = 0x96;
static constexpr uint32_t mmRCC_CONFIG_MEMSIZE = 0xde3;
static constexpr uint32_t mmMP0_SMN_C2PMSG_33  = 0x16061;
static constexpr uint32_t mmIP_DISCOVERY_VERSION = 0x16A00;

static constexpr uint64_t kDiscoveryTmrOffset = 64 << 10;
static constexpr uint32_t kDiscoveryTmrSize   = 10 << 10;
static constexpr uint32_t kDiscoveryMaxSize   = 64 << 10;
static constexpr uint32_t kBinarySignature    = 0x28211407;

namespace {

struct Mmio {
    volatile uint32_t *base;
    uint64_t length;

    uint32_t read(uint32_t reg) const {
        uint64_t off = static_cast<uint64_t>(reg) * 4;
        return off + 4 <= length ? base[reg] : 0xDEADBEEF;
    }
    void write(uint32_t reg, uint32_t value) const {
        uint64_t off = static_cast<uint64_t>(reg) * 4;
        if (off + 4 <= length) base[reg] = value;
    }
    // Same sequence as amdgpu_device_mm_access() for reads.
    uint32_t vramRead32(uint64_t pos) const {
        write(mmMM_INDEX, static_cast<uint32_t>(pos) | 0x80000000);
        write(mmMM_INDEX_HI, static_cast<uint32_t>(pos >> 31));
        return read(mmMM_DATA);
    }
};

void setU32(IOService *s, const char *key, uint32_t v) { s->setProperty(key, v, 32); }
void setU64(IOService *s, const char *key, uint64_t v) { s->setProperty(key, v, 64); }

} // namespace

bool PhxProbe::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-phxprobeoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;

    setU32(this, "phx-vendor-device", pci->configRead32(kIOPCIConfigVendorID));
    setU32(this, "phx-revision-class", pci->configRead32(kIOPCIConfigRevisionID));
    setU32(this, "phx-command", pci->configRead16(kIOPCIConfigCommand));

    // The register BAR is BAR5 on GFX11 parts (BAR0 VRAM, BAR2 doorbells, BAR4 I/O).
    pci->setMemoryEnable(true);
    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!map) {
        setProperty("phx-error", "BAR5 map failed");
        registerService();
        return true;
    }

    Mmio mmio{reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()), map->getLength()};
    setU64(this, "phx-bar5-length", mmio.length);

    uint32_t memsizeMb = mmio.read(mmRCC_CONFIG_MEMSIZE);
    uint32_t c2pmsg33  = mmio.read(mmMP0_SMN_C2PMSG_33);
    uint32_t scratch0  = mmio.read(mmDRIVER_SCRATCH_0);
    uint32_t scratch1  = mmio.read(mmDRIVER_SCRATCH_1);
    uint32_t scratch2  = mmio.read(mmDRIVER_SCRATCH_2);
    setU32(this, "phx-rcc-config-memsize-mb", memsizeMb);
    setU32(this, "phx-mp0-c2pmsg33", c2pmsg33);
    setU32(this, "phx-driver-scratch0", scratch0);
    setU32(this, "phx-driver-scratch1", scratch1);
    setU32(this, "phx-driver-scratch2", scratch2);
    setU32(this, "phx-ip-discovery-version", mmio.read(mmIP_DISCOVERY_VERSION));

    // Locate the discovery table the way amdgpu_discovery_get_tmr_info() does.
    uint64_t offset = 0;
    uint32_t size = kDiscoveryTmrSize;
    uint32_t signature = 0;
    if (memsizeMb != 0 && memsizeMb != 0xFFFFFFFF) {
        offset = (static_cast<uint64_t>(memsizeMb) << 20) - kDiscoveryTmrOffset;
        signature = mmio.vramRead32(offset);
    }
    if (signature != kBinarySignature && scratch2 != 0) {
        offset = (static_cast<uint64_t>(scratch1) << 32) | scratch0;
        size = scratch2;
        signature = mmio.vramRead32(offset);
    }
    setU64(this, "phx-discovery-offset", offset);
    setU32(this, "phx-discovery-signature", signature);

    if (signature == kBinarySignature) {
        if (size > kDiscoveryMaxSize)
            size = kDiscoveryMaxSize;
        size &= ~3u;
        uint32_t *buf = static_cast<uint32_t *>(IOMalloc(size));
        if (buf) {
            for (uint32_t i = 0; i < size / 4; i++)
                buf[i] = mmio.vramRead32(offset + static_cast<uint64_t>(i) * 4);
            OSData *bin = OSData::withBytes(buf, size);
            if (bin) {
                setProperty("phx-discovery-bin", bin);
                bin->release();
            }
            IOFree(buf, size);
        }
    } else {
        setProperty("phx-error", "discovery signature not found");
    }

    IOLog("PhxProbe: memsize=%u MB c2pmsg33=0x%08x discovery@0x%llx sig=0x%08x\n",
          memsizeMb, c2pmsg33, offset, signature);

    map->release();
    registerService();
    return true;
}
