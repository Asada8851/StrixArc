//
//  RDNA3Driver.cpp — Native macOS Kernel Driver implementation for AMD RDNA 3 / Phoenix APU.
//
#include "RDNA3Driver.h"
#include "RDNA3Framebuffer.h"
#include "RDNA3UserClient.h"
#include <IOKit/IOLib.h>

#define DRIVER_LOG(fmt, ...) IOLog("RDNA3Driver: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RDNA3Driver, IOAccelerator)

bool RDNA3Driver::init(OSDictionary *propTable) {
    if (!super::init(propTable)) return false;
    DRIVER_LOG("Initializing RDNA3Driver module (build: native-sonoma-tahoe-accel)");
    return true;
}

IOService *RDNA3Driver::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider, score)) return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t device = pci->configRead16(kIOPCIConfigDeviceID);

    if (vendor != 0x1002) return nullptr;

    // Support Phoenix / Hawk Point GFX1103 families (0x1900, 0x15bf, 0x15c8, 0x164e)
    if (device == 0x1900 || device == 0x15bf || device == 0x15c8 || device == 0x164e) {
        DRIVER_LOG("Probe matched AMD Phoenix/Hawk Point GPU [%04x:%04x]", vendor, device);
        if (score) *score += 2000;
        return this;
    }

    return nullptr;
}

bool RDNA3Driver::start(IOService *provider) {
    if (!super::start(provider)) return false;

    m_pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!m_pciDevice) {
        DRIVER_LOG("Error: Provider is not an IOPCIDevice");
        return false;
    }

    uint16_t vendor = m_pciDevice->configRead16(kIOPCIConfigVendorID);
    uint16_t device = m_pciDevice->configRead16(kIOPCIConfigDeviceID);
    uint8_t  rev    = m_pciDevice->configRead8(kIOPCIConfigRevisionID);

    DRIVER_LOG("Starting driver for AMD GPU [%04x:%04x] Rev 0x%02x", vendor, device, rev);

    // 1. Enable PCI Memory Space and Bus Mastering
    m_pciDevice->setMemoryEnable(true);
    m_pciDevice->setBusMasterEnable(true);

    // 2. Map BAR0 (VRAM / Linear Framebuffer aperture)
    m_bar0Map = m_pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (m_bar0Map) {
        m_vramPhysBase    = m_bar0Map->getPhysicalAddress();
        m_vramSize        = m_bar0Map->getLength();
        m_vramVirtualBase = reinterpret_cast<uint8_t*>(m_bar0Map->getVirtualAddress());
        DRIVER_LOG("BAR0 (VRAM Aperture) mapped: Phys 0x%llx, Size %llu MB",
                   m_vramPhysBase, m_vramSize / (1024 * 1024));
    } else {
        DRIVER_LOG("Warning: Unable to map BAR0 aperture");
    }

    // 3. Map BAR5 (MMIO registers, 512 KiB)
    m_bar5Map = m_pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (m_bar5Map) {
        m_mmioSize        = m_bar5Map->getLength();
        m_mmioVirtualBase = reinterpret_cast<uint8_t*>(m_bar5Map->getVirtualAddress());
        DRIVER_LOG("BAR5 (MMIO Aperture) mapped: Size %llu KB", m_mmioSize / 1024);

        m_mmioBus.configure(m_mmioVirtualBase, m_mmioSize);
        m_devCtx.mmio = &m_mmioBus;
    } else {
        DRIVER_LOG("Warning: Unable to map BAR5 MMIO aperture");
    }

    // 4. Run IP Block Bringup (GMC11, PSP13, SMU13, SDMA6, GFX11)
    if (m_devCtx.mmio) {
        rdna3::MemoryPlanInput planInput{};
        planInput.is_apu          = true;
        planInput.bar0_len        = m_vramSize > 0 ? m_vramSize : (4ull * rdna3::kGiB);
        planInput.bar0_addr       = m_vramPhysBase;
        planInput.bar0_cfg_addr   = m_vramPhysBase;
        planInput.rebar_present   = true;
        planInput.rebar_bytes     = planInput.bar0_len;
        planInput.vram_bytes      = planInput.bar0_len;
        planInput.tmr_reserve     = rdna3::kDefaultTmrReserveBytes;
        planInput.scanout_reserve = rdna3::kDefaultDcnScanoutBytes;
        planInput.console_off     = ~0ULL;
        planInput.console_len     = 0;
        planInput.map_cap         = planInput.bar0_len;

        rdna3::MemoryPlanOutput memPlan = rdna3::plan_memory_aperture(planInput);
        m_devCtx.bar0_phys = m_vramPhysBase;
        m_devCtx.bar0_size = m_vramSize;
        m_devCtx.bar5_phys = m_bar5Map ? m_bar5Map->getPhysicalAddress() : 0;
        m_devCtx.bar5_size = m_mmioSize;
        m_devCtx.vram_size_bytes = memPlan.vram_limit > 0 ? memPlan.vram_limit : (4ull * rdna3::kGiB);
        m_devCtx.vram_mc_base    = 0x0000008000000000ull;

        m_ipBringupActive = rdna3::Rdna3Device::bringup(m_devCtx, memPlan);
        DRIVER_LOG("IP Block bringup (GMC11, PSP13, SMU13, SDMA6, GFX11): %s",
                   m_ipBringupActive ? "SUCCESS" : "DEFERRED");
    }

    // 5. Configure DCN 3.1.4 Scanout Pipeline for 2560x1600 @ 120Hz eDP panel
    m_activeTimings = rdna3::Dcn314Scanout::compute_native_timings();
    DRIVER_LOG("Target eDP Mode: %ux%u @ %uHz (Pixel Clock: %llu Hz)",
               m_activeTimings.h_active, m_activeTimings.v_active,
               m_activeTimings.refresh_rate_hz, m_activeTimings.pixel_clock_hz);

    if (m_devCtx.mmio && m_vramPhysBase != 0) {
        auto regs = rdna3::Dcn314Scanout::generate_register_image(
            m_activeTimings, m_vramPhysBase, m_activeTimings.h_active * 4);
        m_scanoutActive = rdna3::Dcn314Scanout::program_scanout(m_devCtx, regs, 0x0000);
        DRIVER_LOG("DCN 3.1.4 OTG/DCSURF Scanout programmed successfully (active: %d)", m_scanoutActive);
    } else {
        m_scanoutActive = false;
        DRIVER_LOG("DCN 3.1.4 Scanout deferred (hardware MMIO or BAR0 not attached)");
    }

    // 6. Recover our IORegistry Path for Framebuffer Pairing (IOAccelTypes)
    char accelPath[512]{0};
    int  pathLen = sizeof(accelPath);
    bool havePath = getPath(accelPath, &pathLen, gIOServicePlane);
    if (havePath) {
        DRIVER_LOG("Accelerator IORegistry path: %s", accelPath);
    }

    // 7. Instantiate and attach RDNA3Framebuffer
    m_framebuffer = new RDNA3Framebuffer;
    if (m_framebuffer) {
        if (m_framebuffer->init()) {
            m_framebuffer->configure(this, m_vramPhysBase, m_vramSize,
                                     m_activeTimings.h_active,
                                     m_activeTimings.v_active,
                                     m_activeTimings.refresh_rate_hz);

            if (m_framebuffer->attach(m_pciDevice)) {
                if (m_framebuffer->start(m_pciDevice)) {
                    if (havePath) {
                        m_framebuffer->setProperty("IOAccelTypes", accelPath);
                    }
                    DRIVER_LOG("RDNA3Framebuffer attached & started (paired to %s)",
                               havePath ? accelPath : "PCI nub");
                } else {
                    DRIVER_LOG("Error: RDNA3Framebuffer::start failed");
                    m_framebuffer->detach(m_pciDevice);
                    m_framebuffer->release();
                    m_framebuffer = nullptr;
                }
            } else {
                DRIVER_LOG("Error: RDNA3Framebuffer::attach failed");
                m_framebuffer->release();
                m_framebuffer = nullptr;
            }
        } else {
            DRIVER_LOG("Error: RDNA3Framebuffer::init failed");
            m_framebuffer->release();
            m_framebuffer = nullptr;
        }
    }

    // 8. Publish Properties to IORegistry for WindowServer & System Information
    setProperty("model", "AMD Radeon 780M (RDNA 3 / GFX1103)");
    setProperty("device-id", static_cast<uint32_t>(device), 32);
    setProperty("vendor-id", static_cast<uint32_t>(vendor), 32);
    setProperty("revision-id", static_cast<uint32_t>(rev), 32);
    setProperty("VRAM,totalsize", static_cast<uint64_t>(m_vramSize), 64);
    setProperty("Display,width", static_cast<uint32_t>(m_activeTimings.h_active), 32);
    setProperty("Display,height", static_cast<uint32_t>(m_activeTimings.v_active), 32);
    setProperty("Display,refreshRate", static_cast<uint32_t>(m_activeTimings.refresh_rate_hz), 32);
    setProperty("IOAccelIndex", static_cast<uint64_t>(0), 32);
    setProperty("IOAccelRevision", static_cast<uint64_t>(2), 32);

    registerService();
    DRIVER_LOG("RDNA3Driver successfully registered with IOKit registry");
    return true;
}

void RDNA3Driver::stop(IOService *provider) {
    DRIVER_LOG("Stopping RDNA3Driver");

    if (m_framebuffer) {
        m_framebuffer->stop(m_pciDevice);
        m_framebuffer->detach(m_pciDevice);
        m_framebuffer->release();
        m_framebuffer = nullptr;
    }

    m_devCtx.mmio = nullptr;
    m_mmioBus.configure(nullptr, 0);
    if (m_bar5Map) {
        m_bar5Map->release();
        m_bar5Map = nullptr;
    }
    if (m_bar0Map) {
        m_bar0Map->release();
        m_bar0Map = nullptr;
    }
    super::stop(provider);
}

void RDNA3Driver::free() {
    DRIVER_LOG("Freeing RDNA3Driver instance");
    super::free();
}

IOReturn RDNA3Driver::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                    OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;

    RDNA3UserClient *client = new RDNA3UserClient;
    if (!client) return kIOReturnNoMemory;

    if (!client->initWithTask(owningTask, securityID, type)) {
        client->release();
        return kIOReturnError;
    }

    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }

    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}
