//
//  RDNA3Driver.h — Native macOS Kernel Driver for AMD RDNA 3 / Phoenix APU (Radeon 780M / GFX1103)
//  Targeting macOS Sonoma (14.x) and Tahoe (15.x+)
//
#pragma once

#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>

#include "amd/rdna3_dcn314_scanout.h"
#include "amd/rdna3_ip_blocks.h"
#include "amd/rdna3_rebar_uma.h"
#include "uapi/r3n_native_abi.h"

class RDNA3Framebuffer;

class RDNA3Driver : public IOAccelerator {
    OSDeclareDefaultStructors(RDNA3Driver)
    using super = IOAccelerator;

private:
    IOPCIDevice        *m_pciDevice{nullptr};
    IOMemoryMap        *m_bar0Map{nullptr};  // VRAM / FB aperture
    IOMemoryMap        *m_bar5Map{nullptr};  // MMIO aperture
    uint8_t            *m_mmioVirtualBase{nullptr};
    uint64_t            m_mmioSize{0};
    uint8_t            *m_vramVirtualBase{nullptr};
    uint64_t            m_vramPhysBase{0};
    uint64_t            m_vramSize{0};

    class KextMmioBus final : public rdna3::IMmioBus {
    private:
        volatile uint32_t *m_base{nullptr};
        size_t             m_dword_limit{0};

    public:
        KextMmioBus() = default;
        void configure(void *vbase, size_t size_bytes) {
            m_base = static_cast<volatile uint32_t*>(vbase);
            m_dword_limit = size_bytes / sizeof(uint32_t);
        }
        uint32_t read32(uint32_t dword_offset) override {
            if (!m_base || dword_offset >= m_dword_limit) return 0xFFFFFFFFu;
            return m_base[dword_offset];
        }
        void write32(uint32_t dword_offset, uint32_t value) override {
            if (!m_base || dword_offset >= m_dword_limit) return;
            m_base[dword_offset] = value;
        }
        void operator delete(void *, size_t) {}
        void operator delete(void *) {}
    };

    KextMmioBus          m_mmioBus{};
    rdna3::DeviceContext m_devCtx{};
    rdna3::DisplayTimings m_activeTimings{};
    bool                m_scanoutActive{false};
    bool                m_ipBringupActive{false};

    RDNA3Framebuffer   *m_framebuffer{nullptr};

public:
    bool init(OSDictionary *propTable = nullptr) override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;

    // Device accessors for UserClient and Framebuffer
    IOPCIDevice *getPCIDevice() const { return m_pciDevice; }
    uint8_t *getMmioBase() const { return m_mmioVirtualBase; }
    uint64_t getMmioSize() const { return m_mmioSize; }
    uint64_t getVramPhysBase() const { return m_vramPhysBase; }
    uint64_t getVramSize() const { return m_vramSize; }
    const rdna3::DisplayTimings &getActiveTimings() const { return m_activeTimings; }
    bool isScanoutActive() const { return m_scanoutActive; }
    bool isIpBringupActive() const { return m_ipBringupActive; }
    RDNA3Framebuffer *getFramebuffer() const { return m_framebuffer; }
};
