//
//  RDNA3Framebuffer.cpp — Native IOFramebuffer implementation for AMD RDNA 3 / Phoenix APU.
//
#include "RDNA3Framebuffer.h"
#include "RDNA3Driver.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <string.h>

#define FB_LOG(fmt, ...) IOLog("RDNA3Framebuffer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RDNA3Framebuffer, IOFramebuffer)

bool RDNA3Framebuffer::init(OSDictionary *propTable) {
    if (!super::init(propTable)) return false;
    initEdid();
    FB_LOG("Initialized RDNA3Framebuffer instance");
    return true;
}

void RDNA3Framebuffer::initEdid() {
    bzero(m_edid, sizeof(m_edid));

    // VESA EDID 1.4 Header
    m_edid[0] = 0x00; m_edid[1] = 0xFF; m_edid[2] = 0xFF; m_edid[3] = 0xFF;
    m_edid[4] = 0xFF; m_edid[5] = 0xFF; m_edid[6] = 0xFF; m_edid[7] = 0x00;

    // Manufacturer ID: CSW (0x0DAE)
    m_edid[8] = 0x0D; m_edid[9] = 0xAE;
    // Product ID: 0x1600 (Radeon 780M / 1600p panel)
    m_edid[10] = 0x00; m_edid[11] = 0x16;
    // Serial Number: 1
    m_edid[12] = 0x01; m_edid[13] = 0x00; m_edid[14] = 0x00; m_edid[15] = 0x00;
    // Week & Year: Week 1, 2024
    m_edid[16] = 1; m_edid[17] = 34;
    // Version 1, Revision 4
    m_edid[18] = 1; m_edid[19] = 4;
    // Digital DisplayPort input
    m_edid[20] = 0xA5;
    // Physical Dimensions: 34cm x 21cm (16:10 aspect ratio)
    m_edid[21] = 34; m_edid[22] = 21;
    // Gamma: 2.2 ((120 + 100) / 100 = 2.2)
    m_edid[23] = 120;
    // Features: Standby/Suspend, Preferred timing in DTD 1, sRGB
    m_edid[24] = 0x0E;

    // Detailed Timing Descriptor 1: 2560x1600 @ 120Hz (Pixel Clock 537.9072 MHz -> 53790 -> 0xD21E)
    m_edid[54] = 0x1E; m_edid[55] = 0xD2;
    // H Active (2560 = 0xA00) & H Blank (160 = 0x0A0)
    m_edid[56] = 0x00; m_edid[57] = 0xA0; m_edid[58] = 0xA0;
    // V Active (1600 = 0x640) & V Blank (48 = 0x030)
    m_edid[59] = 0x40; m_edid[60] = 0x30; m_edid[61] = 0x60;
    // H Front Porch 48 (0x30), H Sync Width 32 (0x20)
    m_edid[62] = 0x30; m_edid[63] = 0x20;
    // V Front Porch 3, V Sync Width 6
    m_edid[64] = 0x36;
    m_edid[65] = 0x00;
    // Image Size: 344mm x 215mm
    m_edid[66] = 344 & 0xFF; m_edid[67] = 215 & 0xFF;
    m_edid[68] = (((344 >> 8) & 0xF) << 4) | ((215 >> 8) & 0xF);
    // Digital sync flags (positive separate sync)
    m_edid[71] = 0x18;

    // Descriptor 2: Monitor Name "RDNA3 780M eDP"
    m_edid[72] = 0x00; m_edid[73] = 0x00; m_edid[74] = 0x00; m_edid[75] = 0xFC; m_edid[76] = 0x00;
    const char name[] = "RDNA3 780M eDP\n";
    memcpy(&m_edid[77], name, sizeof(name) - 1);
    for (size_t i = 77 + sizeof(name) - 1; i < 90; ++i) m_edid[i] = 0x20;

    // Descriptor 3: Display Range Limits (V: 48-120Hz, H: 30-250kHz, Max 600MHz)
    m_edid[90] = 0x00; m_edid[91] = 0x00; m_edid[92] = 0x00; m_edid[93] = 0xFD; m_edid[94] = 0x00;
    m_edid[95] = 48;  m_edid[96] = 120; // V rate
    m_edid[97] = 30;  m_edid[98] = 250; // H rate
    m_edid[99] = 60;  // Pixel clock limit 600 MHz

    // Extension block count: 0
    m_edid[126] = 0;

    // Checksum: byte 127
    uint32_t csum = 0;
    for (size_t i = 0; i < 127; ++i) csum += m_edid[i];
    m_edid[127] = static_cast<uint8_t>((256 - (csum % 256)) % 256);

    m_edidValid = true;
}

void RDNA3Framebuffer::configure(RDNA3Driver *driver, uint64_t vramPhys, uint64_t vramSize,
                                 uint32_t width, uint32_t height, uint32_t refreshHz) {
    m_driver       = driver;
    m_vramPhysBase = vramPhys;
    m_vramSize     = vramSize;
    m_width        = width ? width : 2560;
    m_height       = height ? height : 1600;
    m_refreshHz    = refreshHz ? refreshHz : 120;
    m_pitchBytes   = m_width * 4;
    m_displayBytes = static_cast<uint64_t>(m_pitchBytes) * m_height;

    FB_LOG("Configured: %ux%u @ %uHz, pitch %u, VRAM Phys 0x%llx (size %llu MB)",
           m_width, m_height, m_refreshHz, m_pitchBytes, m_vramPhysBase, m_vramSize / (1024 * 1024));
}

void RDNA3Framebuffer::initForPM() {
    if (m_pmRegistered) return;
    m_pmRegistered = true;

    static IOPMPowerState powerStates[3] = {
        { 1, 0,                0,            0,            0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0,                0,            kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    for (auto &state : powerStates) state.capabilityFlags |= kIOPMPreventSystemSleep;

    registerPowerDriver(this, powerStates, 3);
    temporaryPowerClampOn();
    changePowerStateTo(2);

    IOService *p = getProvider();
    if (p) p->setProperty("IOPMIsPowerManaged", true);
}

bool RDNA3Framebuffer::start(IOService *provider) {
    m_pciDevice = OSDynamicCast(IOPCIDevice, provider);

    // Publish properties expected by WindowServer and CoreDisplay
    setProperty("IOMatchCategory", "ATIFramebuffer");
    setProperty("IOAccelIndex", (unsigned long long)0, 32);
    setProperty("IOAccelRevision", (unsigned long long)2, 32);
    setProperty("shared-fb", (unsigned long long)1, 32);
    setProperty("display-connectors", (unsigned long long)1, 32);
    setProperty("IOFBMemorySize", (unsigned long long)m_vramSize, 64);

    if (m_edidValid) {
        OSData *edidData = OSData::withBytes(m_edid, sizeof(m_edid));
        if (edidData) {
            setProperty("IODisplayEDID", edidData);
            setProperty("EDID", edidData);
            setProperty("EDID,DDC0", edidData);
            edidData->release();
        }
    }

    if (!super::start(provider)) {
        FB_LOG("Error: super::start failed");
        return false;
    }

    registerService();
    FB_LOG("RDNA3Framebuffer registered successfully with WindowServer/CoreDisplay");
    return true;
}

void RDNA3Framebuffer::stop(IOService *provider) {
    FB_LOG("Stopping RDNA3Framebuffer");
    super::stop(provider);
}

void RDNA3Framebuffer::free() {
    FB_LOG("Freeing RDNA3Framebuffer");
    super::free();
}

IODeviceMemory *RDNA3Framebuffer::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture || m_vramPhysBase == 0 || m_displayBytes == 0) {
        return nullptr;
    }
    return IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(m_vramPhysBase),
                                     static_cast<IOPhysicalLength>(m_displayBytes));
}

IODeviceMemory *RDNA3Framebuffer::getVRAMRange() {
    if (m_vramPhysBase == 0 || m_displayBytes == 0) return nullptr;
    return IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(m_vramPhysBase),
                                     static_cast<IOPhysicalLength>(m_displayBytes));
}

IOReturn RDNA3Framebuffer::enableController() {
    initForPM();
    FB_LOG("enableController invoked");
    return kIOReturnSuccess;
}

const char *RDNA3Framebuffer::getPixelFormats() {
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount RDNA3Framebuffer::getDisplayModeCount() {
    return 1;
}

IOReturn RDNA3Framebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (!allDisplayModes) return kIOReturnBadArgument;
    allDisplayModes[0] = 1;
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) {
    if (!info || displayMode != 1) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth  = m_width;
    info->nominalHeight = m_height;
    info->refreshRate   = (m_refreshHz << 16); // 16.16 fixed point (120.00 Hz)
    info->maxDepthIndex = 0;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

UInt64 RDNA3Framebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    return 0; // Legacy format: must return 0
}

IOReturn RDNA3Framebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                              IOPixelAperture aperture, IOPixelInformation *pixelInfo) {
    if (!pixelInfo || displayMode != 1 || depth != 0 || aperture != kIOFBSystemAperture) {
        return kIOReturnBadArgument;
    }
    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow       = m_pitchBytes;
    pixelInfo->bitsPerPixel      = 32;
    pixelInfo->pixelType         = kIORGBDirectPixels;
    pixelInfo->componentCount    = 3;
    pixelInfo->bitsPerComponent  = 8;
    pixelInfo->componentMasks[0] = 0x00FF0000; // Red
    pixelInfo->componentMasks[1] = 0x0000FF00; // Green
    pixelInfo->componentMasks[2] = 0x000000FF; // Blue

    static const char fmt[] = "--------RRRRRRRRGGGGGGGGBBBBBBBB";
    for (size_t i = 0; i < sizeof(fmt) && i < sizeof(pixelInfo->pixelFormat); ++i) {
        pixelInfo->pixelFormat[i] = fmt[i];
    }
    pixelInfo->activeWidth       = m_width;
    pixelInfo->activeHeight      = m_height;
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    if (displayMode) *displayMode = 1;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    if (displayMode == 1 && depth == 0) return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

IOReturn RDNA3Framebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    if (displayMode) *displayMode = 1;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
    if (attribute == kIOHardwareCursorAttribute) {
        if (value) *value = 0; // Software cursor: highly responsive, zero flicker
        return kIOReturnSuccess;
    }
    return super::getAttribute(attribute, value);
}

IOReturn RDNA3Framebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
    if (attribute == kIOPowerAttribute) {
        m_powerState = static_cast<uint32_t>(value);
        return kIOReturnSuccess;
    }
    return super::setAttribute(attribute, value);
}

IOItemCount RDNA3Framebuffer::getConnectionCount() {
    return 1;
}

IOReturn RDNA3Framebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) {
    if (connectIndex != 0) return kIOReturnBadArgument;

    if (attribute == kConnectionFlags) {
        if (value) *value = kIOConnectionBuiltIn; // Built-in laptop Retina panel
        return kIOReturnSuccess;
    }
    if (attribute == kConnectionEnable || attribute == kConnectionCheckEnable) {
        if (value) *value = 1;
        return kIOReturnSuccess;
    }
    if (attribute == kConnectionSupportsHLDDCSense) {
        return m_edidValid ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    if (attribute == kConnectionSupportsAppleSense || attribute == kConnectionSupportsLLDDCSense) {
        return kIOReturnUnsupported;
    }
    return super::getAttributeForConnection(connectIndex, attribute, value);
}

IOReturn RDNA3Framebuffer::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) {
    if (connectIndex != 0) return kIOReturnBadArgument;
    if (attribute == kConnectionPower) {
        m_connPower = static_cast<uint32_t>(value);
        return kIOReturnSuccess;
    }
    return super::setAttributeForConnection(connectIndex, attribute, value);
}

bool RDNA3Framebuffer::hasDDCConnect(IOIndex connectIndex) {
    return connectIndex == 0 && m_edidValid;
}

IOReturn RDNA3Framebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                      IOSelect blockType, IOOptionBits options,
                                      UInt8 *data, IOByteCount *length) {
    if (connectIndex != 0 || blockNumber != 1 || blockType != kIODDCBlockTypeEDID || !data || !length) {
        return kIOReturnBadArgument;
    }
    if (*length < sizeof(m_edid)) return kIOReturnBadArgument;

    memcpy(data, m_edid, sizeof(m_edid));
    *length = sizeof(m_edid);
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                                void *param1, void *param2, void *param3, void *param4) {
    if (functionName && strcmp(functionName->getCStringNoCopy(), "SpecialAMDKey") == 0) {
        uint32_t type = param1 ? *reinterpret_cast<uint32_t*>(param1) : 0;
        switch (type) {
            case 0x01: // kReqIsDeviceValid
                if (param2) {
                    *reinterpret_cast<uint32_t*>(param2) = 1;
                }
                FB_LOG("Handled SpecialAMDKey: kReqIsDeviceValid -> validated 1");
                return kIOReturnSuccess;

            case 0x16: // kReqAccelStarted
                FB_LOG("Handled SpecialAMDKey: kReqAccelStarted -> announced");
                return kIOReturnSuccess;

            case 0x17: // kReqResetAndReplay
                FB_LOG("Handled SpecialAMDKey: kReqResetAndReplay -> acknowledged");
                return kIOReturnSuccess;

            default:
                FB_LOG("SpecialAMDKey request 0x%x -> unsupported", type);
                return kIOReturnUnsupported;
        }
    }
    return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}
