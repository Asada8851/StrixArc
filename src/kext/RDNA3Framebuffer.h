//
//  RDNA3Framebuffer.h — IOFramebuffer driver for AMD RDNA 3 / Phoenix APU (Radeon 780M / GFX1103)
//  Implements native macOS display scanout, mode configuration, DDC/EDID, and ATIFramebuffer handshake.
//
#pragma once

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>

class RDNA3Driver;

class RDNA3Framebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(RDNA3Framebuffer)
    using super = IOFramebuffer;

private:
    RDNA3Driver        *m_driver{nullptr};
    IOPCIDevice        *m_pciDevice{nullptr};
    uint64_t            m_vramPhysBase{0};
    uint64_t            m_vramSize{0};
    uint32_t            m_width{2560};
    uint32_t            m_height{1600};
    uint32_t            m_refreshHz{120};
    uint32_t            m_pitchBytes{10240};
    uint64_t            m_displayBytes{16384000};
    uint32_t            m_powerState{2};
    uint32_t            m_connPower{1};
    bool                m_pmRegistered{false};
    uint8_t             m_edid[128]{};
    bool                m_edidValid{false};

    void initEdid();
    void initForPM();

public:
    bool init(OSDictionary *propTable = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    void configure(RDNA3Driver *driver, uint64_t vramPhys, uint64_t vramSize,
                   uint32_t width, uint32_t height, uint32_t refreshHz);

    // IOFramebuffer scanout and memory management
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;
    IOReturn enableController() override;

    // Display modes
    const char *getPixelFormats() override;
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;

    // Attributes
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;

    // Connection & DDC/EDID
    IOItemCount getConnectionCount() override;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) override;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) override;
    bool hasDDCConnect(IOIndex connectIndex) override;
    IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits options, UInt8 *data, IOByteCount *length) override;

    // Apple GraphicsAccelerator peer handshake
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                  void *param1, void *param2, void *param3, void *param4) override;
};
