//
//  RDNA3UserClient.cpp — IOUserClient implementation for RDNA 3 driver.
//
#include "RDNA3UserClient.h"
#include <IOKit/IOLib.h>
#include <string.h>

#define CLIENT_LOG(fmt, ...) IOLog("RDNA3UserClient: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RDNA3UserClient, IOUserClient)

bool RDNA3UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!super::initWithTask(owningTask, securityID, type)) return false;
    m_owningTask = owningTask;
    m_clientAlive = true;
    bzero(m_bos, sizeof(m_bos));
    CLIENT_LOG("Initialized RDNA3UserClient instance for task %p (type 0x%x)", owningTask, type);
    return true;
}

bool RDNA3UserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    m_driver = OSDynamicCast(RDNA3Driver, provider);
    if (!m_driver) {
        CLIENT_LOG("Error: Provider is not an RDNA3Driver");
        return false;
    }
    CLIENT_LOG("RDNA3UserClient connected to RDNA3Driver");
    return true;
}

void RDNA3UserClient::stop(IOService *provider) {
    CLIENT_LOG("Stopping RDNA3UserClient");
    m_clientAlive = false;
    m_driver = nullptr;
    super::stop(provider);
}

IOReturn RDNA3UserClient::clientClose() {
    CLIENT_LOG("RDNA3UserClient clientClose called");
    terminate();
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleHello(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!out || !outSize || *outSize < sizeof(uint32_t)) return kIOReturnBadArgument;
    *reinterpret_cast<uint32_t*>(out) = 0x52334E31u; // 'R3N1'
    *outSize = sizeof(uint32_t);
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleQueryInfo(void *out, size_t *outSize) {
    if (!out || !outSize || *outSize < sizeof(r3n_info)) return kIOReturnBadArgument;
    if (!m_driver) return kIOReturnNotAttached;

    const auto &timings = m_driver->getActiveTimings();

    r3n_info info = {};
    info.chip_id            = 0x1900;
    info.chip_rev           = 0xBA;
    info.family             = 143; // AMD_FAMILY_GC_11_0_3
    info.ip_version_major   = 11;
    info.ip_version_minor   = 0;
    info.ip_version_rev     = 3;
    info.vram_size_bytes    = 4ull * 1024 * 1024 * 1024;
    info.vram_visible_bytes = m_driver->getVramSize() ? m_driver->getVramSize() : 256ull * 1024 * 1024;
    info.gtt_size_bytes     = 8ull * 1024 * 1024 * 1024;
    info.cu_count           = 12; // Radeon 780M has 12 Compute Units
    info.max_engine_clock_mhz = 2700;
    info.max_memory_clock_mhz = 3200;
    info.display_width      = timings.h_active ? timings.h_active : 2560;
    info.display_height     = timings.v_active ? timings.v_active : 1600;
    info.display_refresh_hz = timings.refresh_rate_hz ? timings.refresh_rate_hz : 120;

    *reinterpret_cast<r3n_info*>(out) = info;
    *outSize = sizeof(r3n_info);
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleBoCreate(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(r3n_gem_create_in) || !out || !outSize || *outSize < sizeof(uint64_t)) {
        return kIOReturnBadArgument;
    }

    const auto *req = reinterpret_cast<const r3n_gem_create_in*>(in);
    uint64_t allocSize = (req->size_bytes + 4095) & ~4095ull;

    // Find free slot
    for (size_t i = 0; i < kMaxTrackedBos; ++i) {
        if (!m_bos[i].active) {
            m_bos[i].handle  = m_nextBoHandle++;
            m_bos[i].size    = allocSize;
            m_bos[i].domain  = req->domain;
            m_bos[i].gpu_va  = 0;
            m_bos[i].active  = true;

            struct OutStruct {
                uint32_t handle;
                uint32_t reserved;
                uint64_t allocated_size;
            };
            auto *res = reinterpret_cast<OutStruct*>(out);
            res->handle = m_bos[i].handle;
            res->allocated_size = allocSize;
            *outSize = sizeof(OutStruct);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNoMemory;
}

IOReturn RDNA3UserClient::handleBoFree(const void *in, size_t inSize) {
    if (!in || inSize < sizeof(uint32_t)) return kIOReturnBadArgument;
    uint32_t handle = *reinterpret_cast<const uint32_t*>(in);

    for (size_t i = 0; i < kMaxTrackedBos; ++i) {
        if (m_bos[i].active && m_bos[i].handle == handle) {
            m_bos[i].active = false;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNotFound;
}

IOReturn RDNA3UserClient::handleGemVa(const void *in, size_t inSize) {
    if (!in || inSize < sizeof(r3n_gem_va)) return kIOReturnBadArgument;
    const auto *req = reinterpret_cast<const r3n_gem_va*>(in);

    for (size_t i = 0; i < kMaxTrackedBos; ++i) {
        if (m_bos[i].active && m_bos[i].handle == req->handle) {
            if (req->op == R3N_VA_OP_MAP) {
                m_bos[i].gpu_va = req->gpu_va;
            } else if (req->op == R3N_VA_OP_UNMAP) {
                m_bos[i].gpu_va = 0;
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNotFound;
}

IOReturn RDNA3UserClient::handleCtx(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(r3n_ctx) || !out || !outSize || *outSize < sizeof(r3n_ctx)) {
        return kIOReturnBadArgument;
    }
    const auto *req = reinterpret_cast<const r3n_ctx*>(in);
    auto *res = reinterpret_cast<r3n_ctx*>(out);

    if (req->op == 1) { // Create
        res->ctx_id = 1; // Default hardware graphics ring context
        res->op     = 1;
        *outSize = sizeof(r3n_ctx);
        return kIOReturnSuccess;
    } else if (req->op == 2) { // Destroy
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn RDNA3UserClient::handleSubmit(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(r3n_cs_in) || !out || !outSize || *outSize < sizeof(uint64_t)) {
        return kIOReturnBadArgument;
    }
    const auto *cs = reinterpret_cast<const r3n_cs_in*>(in);
    (void)cs;

    // Advance fence sequence
    m_lastSeqno++;
    *reinterpret_cast<uint64_t*>(out) = m_lastSeqno;
    *outSize = sizeof(uint64_t);

    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleWaitSeq(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(uint64_t) || !out || !outSize || *outSize < sizeof(uint32_t)) {
        return kIOReturnBadArgument;
    }
    uint64_t seq = *reinterpret_cast<const uint64_t*>(in);
    *reinterpret_cast<uint32_t*>(out) = (seq <= m_lastSeqno) ? 1 : 0;
    *outSize = sizeof(uint32_t);
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleReadRegs(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(uint32_t) || !out || !outSize || *outSize < sizeof(uint32_t)) {
        return kIOReturnBadArgument;
    }
    if (!m_driver) return kIOReturnNotAttached;

    uint32_t dwordOffset = *reinterpret_cast<const uint32_t*>(in);
    uint8_t *mmio = m_driver->getMmioBase();
    uint64_t mmioSize = m_driver->getMmioSize();

    if (!mmio || (dwordOffset * sizeof(uint32_t) + sizeof(uint32_t)) > mmioSize) {
        return kIOReturnBadArgument;
    }

    volatile uint32_t *regPtr = reinterpret_cast<volatile uint32_t*>(mmio);
    *reinterpret_cast<uint32_t*>(out) = regPtr[dwordOffset];
    *outSize = sizeof(uint32_t);
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleScanQuery(void *out, size_t *outSize) {
    if (!out || !outSize || *outSize < sizeof(r3n_scan_reg)) return kIOReturnBadArgument;
    if (!m_driver) return kIOReturnNotAttached;

    const auto &timings = m_driver->getActiveTimings();

    r3n_scan_reg sr = {};
    sr.bo_handle    = 1;
    sr.width        = timings.h_active ? timings.h_active : 2560;
    sr.height       = timings.v_active ? timings.v_active : 1600;
    sr.stride_bytes = sr.width * 4;
    sr.format       = 0x00000004; // ARGB8888

    *reinterpret_cast<r3n_scan_reg*>(out) = sr;
    *outSize = sizeof(r3n_scan_reg);
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::handleScanPresent(const void *in, size_t inSize, void *out, size_t *outSize) {
    if (!in || inSize < sizeof(r3n_scan_present)) return kIOReturnBadArgument;
    if (!m_driver) return kIOReturnNotAttached;

    const auto *req = reinterpret_cast<const r3n_scan_present*>(in);
    if (out && outSize && *outSize >= sizeof(uint32_t)) {
        *reinterpret_cast<uint32_t*>(out) = req->slot_id;
        *outSize = sizeof(uint32_t);
    }
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                         IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    if (!m_clientAlive || !m_driver) return kIOReturnNotAttached;
    if (!args) return kIOReturnBadArgument;

    switch (selector) {
        case R3N_SEL_HELLO:
            return handleHello(args->structureInput, args->structureInputSize,
                               args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_QUERYINFO:
            return handleQueryInfo(args->structureOutput,
                                   reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_BOCREATE:
            return handleBoCreate(args->structureInput, args->structureInputSize,
                                  args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_BOFREE:
            return handleBoFree(args->structureInput, args->structureInputSize);

        case R3N_SEL_GEMVA:
            return handleGemVa(args->structureInput, args->structureInputSize);

        case R3N_SEL_CTX:
            return handleCtx(args->structureInput, args->structureInputSize,
                             args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_SUBMIT:
            return handleSubmit(args->structureInput, args->structureInputSize,
                                args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_WAITSEQ:
            return handleWaitSeq(args->structureInput, args->structureInputSize,
                                 args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_READREGS:
            return handleReadRegs(args->structureInput, args->structureInputSize,
                                  args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_SCAN_QUERY:
            return handleScanQuery(args->structureOutput,
                                   reinterpret_cast<size_t*>(&args->structureOutputSize));

        case R3N_SEL_SCAN_PRESENT:
            return handleScanPresent(args->structureInput, args->structureInputSize,
                                     args->structureOutput, reinterpret_cast<size_t*>(&args->structureOutputSize));

        default:
            CLIENT_LOG("Unsupported selector 0x%x", selector);
            return kIOReturnUnsupported;
    }
}
