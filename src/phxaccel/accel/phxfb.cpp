// PhxFB: the 780M's display as an IOFramebuffer that PhxAccel (NMXAccel) composites for WindowServer.
// Like NullMoth's NMIntelFB (phase 1), it takes over the mode the firmware (GOP) already lit, at boot in place of the
// firmware framebuffer (IONDRV), and writes nothing to the display engine: the scan-out surface is the boot console's
// (IOPlatformExpert::getConsoleInfo), the accelerator's display pipe copies each finished frame into a write-combined CPU
// mapping of it, and a timer at the panel's refresh (measured on the display engine, measureRefresh) stands in for the VBL
// interrupt WindowServer paces an accelerated display on.
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "rdna3_chip.h"
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include "nvrm_vram_abi.h"
#include "nvrm_gpuva_abi.h"

void phxAgdcInstall();   // phxagdc.cpp: the AppleGraphicsDeviceControl that reports this panel (debug.phxagdc)
void phxAgdcRemove();
void phxHoldInit(IOService *fb, unsigned bootHoldSec);   // phxagdc.cpp: debug.phxfb_hold keeps this framebuffer (and so the GPU) busy

#define PFBLOG(fmt, ...) IOLog("PhxFB: " fmt "\n", ##__VA_ARGS__)

static int gAlias = 1;
static bool gAliasSysctl;
SYSCTL_INT(_debug, OID_AUTO, phxfb_alias, CTLFLAG_RW | CTLFLAG_LOCKED, &gAlias, 0,
           "1 = frames are copied into the scan-out through its system-RAM address (the carve-out), 0 = through the GPU's BAR");
// Which CPU view of the scan-out the display pipe writes frames into. The carve-out view asked write-combined came out
// write-back on the 780M (fast), but on the 880M a 1920x1200 frame took 54 ms to copy (~170 MB/s, the speed of
// uncached stores; reading the source took 0.7 ms), so the composited desktop ran at 18 fps (2026-10-26). So the
// carve-out is mapped asked write-combined, then asked write-back, one at a time (two cache types on one physical page
// at once is undefined on x86), each one's write speed is measured (the pixels already there written back over
// themselves: the panel does not change), and the faster stays mapped until stop(); the BAR is measured too.
// debug.phxfb_alias 0/1 still switches between the BAR and that view at any frame.
static int gAliasMBps[3];   // BAR, carve-out asked write-combined, carve-out asked write-back; 0 = not measured/unusable
static int gAliasView;      // 0 none (BAR), 1 write-combined, 2 write-back
SYSCTL_INT(_debug, OID_AUTO, phxfb_mbps_bar, CTLFLAG_RD | CTLFLAG_LOCKED, &gAliasMBps[0], 0, "measured write speed into the scan-out through the BAR, MB/s");
SYSCTL_INT(_debug, OID_AUTO, phxfb_mbps_wc, CTLFLAG_RD | CTLFLAG_LOCKED, &gAliasMBps[1], 0, "measured write speed through the carve-out asked write-combined, MB/s");
SYSCTL_INT(_debug, OID_AUTO, phxfb_mbps_wb, CTLFLAG_RD | CTLFLAG_LOCKED, &gAliasMBps[2], 0, "measured write speed through the carve-out asked write-back, MB/s");
SYSCTL_INT(_debug, OID_AUTO, phxfb_alias_view, CTLFLAG_RD | CTLFLAG_LOCKED, &gAliasView, 0, "the carve-out view kept: 0 none (frames through the BAR), 1 write-combined, 2 write-back");

// The VBL tick. XNU's timer coalescing moves a kernel deadline by up to an eighth of its delay, at most 1 ms, so a
// 120 Hz tick could land anywhere in the first millisecond after the vblank it stands for. The system-critical urgency
// that is never moved is not exported to kexts (thread_call_enter_delayed_with_leeway), so the tick is armed 1.5 ms
// early and re-armed for its deadline from there, each stage with an eighth of the previous slop, until less than
// 50 us remain (debug.phxfb_vbl_staged, 0 = one plain deadline). The lateness of each tick is summed for the
// measurements, and nvrm-accel.cpp times its presents against the last tick (gPhxVblLastAbs, gPhxFrameAbs).
static int gVblStaged = 1;
static SInt64 gVblN, gVblLateUsSum, gVblLateUsMax;
uint64_t gPhxVblLastAbs, gPhxFrameAbs;
volatile uint64_t gPhxVblSeq;   // ticks so far, counted before the handlers run: the display pipe completes on the next one
SYSCTL_INT(_debug, OID_AUTO, phxfb_vbl_staged, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblStaged, 0,
           "1 = the VBL tick is armed early and re-armed until its deadline (little coalescing slop), 0 = one plain deadline");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gVblN, "VBL ticks fired");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_sum, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsSum, "microseconds the VBL ticks fired past their deadlines, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsMax, "the latest VBL tick, microseconds past its deadline (writable: 0 restarts it)");
static struct sysctl_oid *const kFbOids[] = { &sysctl__debug_phxfb_alias, &sysctl__debug_phxfb_mbps_bar, &sysctl__debug_phxfb_mbps_wc,
                                              &sysctl__debug_phxfb_mbps_wb, &sysctl__debug_phxfb_alias_view,
                                              &sysctl__debug_phxfb_vbl_staged, &sysctl__debug_phxfb_vbl_n,
                                              &sysctl__debug_phxfb_vbl_late_us_sum, &sysctl__debug_phxfb_vbl_late_us_max };

// The class name carries "AMD": CoreDisplay tells the GPU vendor from the framebuffer's class name (Intel, AMD, NVDA,
// AppleParavirt), and for any other name it turns IOPresentment off, which left the panel on the software compositor
// with the Metal compositor up for the accelerator (GetGPUVendorForFramebufferService -> 1, 2026-10-20).
class PhxAMDFB : public IOFramebuffer {
    OSDeclareDefaultStructors(PhxAMDFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface through the BAR
    // The same surface through its system-RAM address: the 780M's VRAM is a carve-out of system memory, and amdgpu maps
    // it there on APUs rather than through the BAR. Through the BAR a 2560x1600 frame took ~110 ms to copy (~7 fps, the
    // whole desktop lagged, 2026-10-20). This view comes out write-back although it asks for write-combined (frames
    // copied with plain stores stayed in the CPU cache: stripes), so the display pipe writes it with non-temporal stores.
    IOMemoryMap *fAliasMap = nullptr;   // the carve-out view kept (gAliasView says which cache request)
    // GPU copies (PhxAccel's debug.nvaccel_iop_gpucopy): PhxBringup does them ("phxScanoutCopy", build 36: SDMA into a
    // second buffer and a flip), told where the firmware's frame is (its BAR offset: the BAR maps the carve-out from 0)
    IOService *fBringup = nullptr;
    unsigned fBringupTries = 0;
    uint64_t fScanBarOff = ~0ull;
    unsigned fAliasTries = 0;
    volatile UInt32 fAliasBusy = 0;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl[4], fConnect;   // IOFramebuffer itself and, when it asks, IOAccelDisplayPipe each register a VBL handler
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    bool fBeside = false;   // another framebuffer (the firmware's IONDRV one) already drives this GPU: not the console
    IOPhysicalAddress fBase = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0;
    static constexpr uint64_t kFrameNs = 16666667ull;   // 60 Hz, when the refresh cannot be measured
    uint64_t fFrameNs = kFrameNs;
    uint32_t fRefreshMilliHz = 60000, fHTotal = 0, fVTotal = 0;
    static constexpr unsigned kBootHoldSec = 120;   // the boot daemon releases it sooner (debug.phxfb_hold=0)
    // The accelerator is NullMoth's, written against an NVIDIA RM: it backs each IOAccel surface with "VRAM" from the
    // framebuffer (nvAllocVram) and a GPU address from the RM (nvGpuVaAlloc/Map). On the 780M nothing on the GPU reads
    // those surfaces -- the display pipe copies finished frames by CPU, and RADV keeps its own GPU addresses in
    // PhxBringup -- so VRAM here is physically contiguous system memory (the 780M has no other kind) and a GPU address
    // is a number from a private range that never enters a page table. Without them every surface_flush WindowServer
    // makes failed ("failed to alloc GART space"), 30 times a second, and no frame reached the panel (2026-10-20).
    static constexpr uint64_t kVaBase = 0x100000000000ull, kVaSize = 1ull << 40, kVramCap = 1024ull << 20;
    IORangeAllocator *fVa = nullptr;
    volatile SInt64 fVramBytes = 0;
    volatile SInt32 fHandles = 0;
    unsigned fRmLogged = 0;
    // VRAM comes from one physically contiguous pool taken at start, while memory is still unfragmented: allocated one
    // by one, a 16 MB surface found no contiguous run hours into a session, and when WindowServer re-allocated its frame
    // on display wake the panel kept the frame from before the display slept (2026-10-21). An allocation is a
    // sub-descriptor of the pool (its handle); past the pool each one is contiguous memory of its own, as before.
    static constexpr uint64_t kPoolTry[3] = { 512ull << 20, 256ull << 20, 128ull << 20 };
    IOBufferMemoryDescriptor *fPool = nullptr;
    IORangeAllocator *fPoolRange = nullptr;
    uint8_t *fPoolKva = nullptr;
    uint64_t fPoolPhys = 0, fPoolBytes = 0;

    void poolInit() {
        for (uint64_t bytes : kPoolTry) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
            if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
            if (!b) continue;
            fPoolRange = IORangeAllocator::withRange(bytes, PAGE_SIZE, 0, IORangeAllocator::kLocking);
            if (!fPoolRange) { b->complete(); b->release(); return; }
            fPool = b; fPoolKva = (uint8_t *)b->getBytesNoCopy(); fPoolBytes = bytes;
            fPoolPhys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            PFBLOG("VRAM pool: %llu MB at phys 0x%llx", bytes >> 20, fPoolPhys);
            return;
        }
        PFBLOG("VRAM pool: no contiguous run of %llu MB or less, every allocation on its own", kPoolTry[2] >> 20);
    }
    void poolFree() {
        OSSafeReleaseNULL(fPoolRange);
        if (fPool) { fPool->complete(); OSSafeReleaseNULL(fPool); }
        fPoolKva = nullptr; fPoolPhys = fPoolBytes = 0;
    }
    bool poolAlloc(NVRMVramRequest *r, uint64_t bytes) {
        IORangeScalar off = 0;
        if (!fPoolRange || !fPoolRange->allocate(bytes, &off, PAGE_SIZE)) return false;
        IOSubMemoryDescriptor *s = IOSubMemoryDescriptor::withSubRange(fPool, off, bytes, kIODirectionInOut);
        if (!s) { fPoolRange->deallocate(off, bytes); return false; }
        bzero(fPoolKva + off, bytes);
        r->handle = s; r->kva = fPoolKva + off; r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = fPoolPhys + off;
        return true;
    }

    IOReturn vramAlloc(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION || !r->size) return kIOReturnBadArgument;
        const uint64_t bytes = round_page_64(r->size);
        if ((uint64_t)OSAddAtomic64((SInt64)bytes, &fVramBytes) + bytes > kVramCap) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes refused, %lld already out (cap %llu)", bytes, fVramBytes, kVramCap);
            return kIOReturnNoMemory;
        }
        if (poolAlloc(r, bytes)) {
            r->mappedTotal = (uint64_t)fVramBytes;
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx from the pool (%lld out)", bytes, r->phys, fVramBytes);
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
        if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
        if (!b) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: no contiguous %llu bytes", bytes);
            return kIOReturnNoMemory;
        }
        bzero(b->getBytesNoCopy(), bytes);
        r->handle = b; r->kva = b->getBytesNoCopy(); r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        r->mappedTotal = (uint64_t)fVramBytes;
        if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx (%lld out)", bytes, r->phys, fVramBytes);
        return kIOReturnSuccess;
    }
    IOReturn vramFree(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (IOSubMemoryDescriptor *s = OSDynamicCast(IOSubMemoryDescriptor, (OSObject *)r->handle)) {
            const uint64_t off = (uint64_t)((uint8_t *)r->kva - fPoolKva), len = s->getLength();
            if (!fPoolRange || (uint8_t *)r->kva < fPoolKva || off + len > fPoolBytes) return kIOReturnBadArgument;
            OSAddAtomic64(-(SInt64)len, &fVramBytes);
            fPoolRange->deallocate(off, len);
            s->release();
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)r->handle);
        if (!b) return kIOReturnBadArgument;
        OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes);
        b->complete(); b->release();
        return kIOReturnSuccess;
    }
    IOReturn rmCall(const OSSymbol *fn, void *p1) {
        if (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES)) {
            NVRMRmHandles *h = (NVRMRmHandles *)p1;
            if (!h || h->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
            h->kapiDevice = this; h->hClient = 0x50480001; h->hDevice = 0x50480002; h->hSubDevice = 0x50480003;
            return kIOReturnSuccess;
        }
        NVRMGpuVaRequest *g = (NVRMGpuVaRequest *)p1;
        if (!g || g->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
        if (fn->isEqualTo(NVRM_GPUVA_FN_ALLOC)) {
            const uint64_t len = round_page_64(g->size);
            uint64_t align = g->align > PAGE_SIZE ? g->align : PAGE_SIZE;
            if (align & (align - 1)) align = PAGE_SIZE;
            IORangeScalar off = 0;
            if (!fVa || !len || !fVa->allocate(len, &off, align)) { g->status = 1; return kIOReturnNoSpace; }
            g->gpuva = kVaBase + off; g->hVirt = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_MAP)) {   // nothing to map: no GPU work reaches these surfaces
            g->hMemory = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_FREE)) {
            if (!(g->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY) && fVa && g->gpuva >= kVaBase && g->gpuva - kVaBase < kVaSize)
                fVa->deallocate(g->gpuva - kVaBase, round_page_64(g->size));
            g->status = 0;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }

    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        PhxAMDFB *me = (PhxAMDFB *)p0;
        if (me->fStopping) return;
        const uint64_t at = mach_absolute_time(), due = me->fVblNext;
        uint64_t near; nanoseconds_to_absolutetime(50000, &near);
        if (gVblStaged && due > at + near) { thread_call_enter_delayed(me->fVblTimer, due); return; }   // an early stage
        if (due && at > due) {
            uint64_t ns; absolutetime_to_nanoseconds(at - due, &ns);
            const SInt64 us = (SInt64)(ns / 1000);
            OSAddAtomic64(us, &gVblLateUsSum);
            if (us > gVblLateUsMax) gVblLateUsMax = us;
        }
        OSAddAtomic64(1, &gVblN);
        gPhxVblLastAbs = due ? due : at;   // the vblank this tick stands for
        __atomic_add_fetch(&gPhxVblSeq, 1, __ATOMIC_RELEASE);
        for (const Irq &q : me->fVbl) {
            const IOFBInterruptProc proc = __atomic_load_n(&q.proc, __ATOMIC_ACQUIRE);
            if (proc) proc(q.target, q.ref);
        }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        const uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        uint64_t lead; nanoseconds_to_absolutetime(1500000, &lead);
        thread_call_enter_delayed(me->fVblTimer, gVblStaged ? me->fVblNext - lead : me->fVblNext);
    }

    // The firmware lit the panel at its native refresh, which the VBL timer and the mode must report: they said 60 Hz while
    // the panel scanned at 120 (OTG frame counter: 392 frames in 3.28 s), and WindowServer composited at 60 fps (2026-10-20).
    // Read-only: OTG0's H/V totals, control and frame counter (DCN 3.1.4 on Phoenix, 3.5 on Strix Point, where OTG_CONTROL
    // and the frame counter sit one dword higher; src/amd/rdna3_chip.h) over 30 frames, snapped to a standard rate when
    // within 0.5 %.
    void measureRefresh() {
        const rdna3_chip *chip = fPCI ? rdna3_chip_for_pci(fPCI->configRead16(kIOPCIConfigDeviceID)) : nullptr;
        if (!chip) { PFBLOG("refresh: unknown GPU, 60 Hz assumed"); return; }
        const rdna3_otg_regs &o = chip->otg0;
        IOMemoryMap *mm = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
        if (!mm || mm->getLength() < (o.frame_count + 1) * 4) { OSSafeReleaseNULL(mm); PFBLOG("refresh: no MMIO view, 60 Hz assumed"); return; }
        volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
        const uint32_t ctl = r[o.control], ht = r[o.h_total] + 1, vt = r[o.v_total] + 1;
        // Timed between frame-counter edges, not over a fixed sleep: 12 or 13 frames in 200 ms read 60.0 or 64.8 Hz on
        // the 880M's panel (2026-10-26), and the VBL timer then ran up to 8 % off the panel. The counter is polled every
        // 50 us; its first change starts the clock and the 30th after it stops it (~0.5 s at 60 Hz, ~0.01 % error).
        uint32_t c = r[o.frame_count] & 0xffffff, frames = 0;
        uint64_t t0 = 0, t1 = 0, deadline;
        clock_interval_to_deadline(1500, kMillisecondScale, &deadline);
        while (mach_absolute_time() < deadline) {
            IODelay(50);
            const uint32_t n = r[o.frame_count] & 0xffffff;
            if (n == c) continue;
            const uint64_t now = mach_absolute_time();
            const uint32_t d = (n - c) & 0xffffff;
            c = n;
            if (!t0) { t0 = now; continue; }
            frames += d;
            if (frames >= 30) { t1 = now; break; }
        }
        mm->release();
        uint64_t ns = 0;
        if (t1) absolutetime_to_nanoseconds(t1 - t0, &ns); else frames = 0;
        if (!(ctl & 1) || !ns || frames < 4 || ht < 100 || vt < 100) { PFBLOG("refresh: OTG0 not running (ctl 0x%x, %u frames), 60 Hz assumed", ctl, frames); return; }
        uint32_t mhz = (uint32_t)(((uint64_t)frames * 1000000000000ull) / ns);
        static const uint32_t kStd[] = { 48000, 50000, 60000, 75000, 90000, 100000, 120000, 144000, 165000, 240000 };
        for (uint32_t k : kStd) if (mhz > k * 199 / 200 && mhz < k * 201 / 200) { mhz = k; break; }
        if (mhz < 24000 || mhz > 360000) { PFBLOG("refresh: %u mHz measured, out of range - 60 Hz assumed", mhz); return; }
        fRefreshMilliHz = mhz; fHTotal = ht; fVTotal = vt;
        fFrameNs = 1000000000000ull / mhz;
        PFBLOG("refresh: OTG0 %ux%u totals, %u frames in %llu us -> %u.%03u Hz", ht, vt, frames, ns / 1000, mhz / 1000, mhz % 1000);
    }

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return !fBeside; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight);
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (mode != 1 || !info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
        info->refreshRate = (IOFixed1616)(((uint64_t)fRefreshMilliHz << 16) / 1000); info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fRowBytes; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fWidth; pi->activeHeight = fHeight;
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;   // no detailed timing: the panel keeps the firmware's
        return kIOReturnSuccess;
    }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) {
            for (Irq &q : fVbl) if (!q.proc) {
                q.target = target; q.ref = ref; __atomic_store_n(&q.proc, proc, __ATOMIC_RELEASE);
                PFBLOG("VBL handler %u: %s", (unsigned)(&q - fVbl), target ? target->getMetaClass()->getClassName() : "(no target)");
                if (iref) *iref = &q;
                return kIOReturnSuccess;
            }
            PFBLOG("VBL handler refused: all %u taken", (unsigned)(sizeof fVbl / sizeof fVbl[0]));
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        for (Irq &q : fVbl) if (iref == &q) __atomic_store_n(&q.proc, (IOFBInterruptProc)nullptr, __ATOMIC_RELEASE);
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // power and connection answers as NullMoth's NMIntelFB/NVRMFB give them: acknowledge at once, never queue on the
    // controller thread (a base-class power change waited 45 s for an acknowledge and froze WindowServer restarts)
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0 || !value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    // The accelerator's display pipe asks for the scan-out: its CPU mapping (frames are copied in) and its physical
    // address in the GPU's VRAM aperture. With no address the pipe got no framebuffer resource, and IOAcceleratorFamily
    // marks a display pipe active only once one exists (init_framebuffer_resource sets +0x298): every WindowServer
    // transaction ended kIOReturnNotReady and no frame reached the panel (2026-10-20).
    // The scan-out through the carve-out, once PhxBringup has published where the carve-out sits (phx-fb-phys) and only
    // if a view holds the pixels the BAR shows. Until then (and if neither view does) frames go through the BAR.
    IOMemoryMap *carveoutView(uint64_t phys, uint64_t bytes, IOOptionBits cache, const char *what) {
        IOMemoryMap *m = nullptr;
        if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { m = dm->map(cache); dm->release(); }
        if (!m) { PFBLOG("alias %s: system RAM 0x%llx would not map", what, phys); return nullptr; }
        const volatile uint32_t *a = (const volatile uint32_t *)m->getVirtualAddress();
        const volatile uint32_t *b = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
        const uint64_t words = bytes / 4, step = words / 4096 ? words / 4096 : 1;
        unsigned same = 0, nz = 0, n = 0;
        for (uint64_t w = 0; w < words; w += step, n++) { const uint32_t x = a[w], y = b[w]; same += x == y; nz += y != 0; }
        PFBLOG("alias %s: scan-out = system RAM 0x%llx: %u of %u samples match the BAR (%u non-zero)", what, phys, same, n, nz);
        if (same == n && nz) return m;
        m->release();
        return nullptr;
    }
    // MB/s of non-temporal 8-byte stores (the display pipe's copy loop) into a view: the scan-out's first bytes, read
    // once through the BAR and written back unchanged, so the panel shows nothing. Called from the scan-out request,
    // just before the pipe copies its frame, so no frame copy runs meanwhile.
    static uint32_t writeMBps(IOMemoryMap *view, const uint64_t *pixels, uint64_t bytes) {
        uint64_t *d = (uint64_t *)view->getVirtualAddress();
        const uint64_t n = bytes / 8, t0 = mach_absolute_time();
        for (uint64_t i = 0; i < n; i++) __asm__ __volatile__("movnti %1, %0" : "=m"(d[i]) : "r"(pixels[i]));
        __asm__ __volatile__("sfence" ::: "memory");
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        return ns ? (uint32_t)(bytes * 1000ull / ns) : 0;
    }
    void aliasSetup() {
        if (fAliasTries >= 3) return;
        if (!OSCompareAndSwap(0, 1, &fAliasBusy)) return;
        fAliasTries++;
        uint64_t fbPhys = 0, barOff = ~0ull;
        if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject()) {
                IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
                OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty("phx-fb-phys")) : nullptr;
                if (n) { fbPhys = n->unsigned64BitValue(); break; }
            }
            it->release();
        }
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                barOff = fBase - dm->getPhysicalAddress();
                break;
            }
        }
        if (!fbPhys || barOff == ~0ull) {
            PFBLOG("alias: carve-out 0x%llx, scan-out BAR offset 0x%llx: no system-RAM view yet, copying through the BAR", fbPhys, barOff);
            fAliasBusy = 0;
            return;
        }
        const uint64_t phys = fbPhys + barOff, bytes = (uint64_t)fRowBytes * fHeight;
        const uint64_t probe = bytes < (1u << 20) ? bytes & ~7ull : 1u << 20;
        uint64_t *px = probe ? (uint64_t *)IOMalloc(probe) : nullptr;
        if (px) {
            const volatile uint64_t *bar = (const volatile uint64_t *)fSurfMap->getVirtualAddress();
            for (uint64_t i = 0; i < probe / 8; i++) px[i] = bar[i];
            gAliasMBps[0] = (int)writeMBps(fSurfMap, px, probe);
        }
        // write-combined first (the view the 780M has always used), checked against the BAR
        IOMemoryMap *wc = carveoutView(phys, bytes, kIOMapWriteCombineCache, "write-combined");
        if (!wc && fAliasTries < 3) {   // e.g. an all-black first frame: nothing to compare yet, try again at a later one
            if (px) IOFree(px, probe);
            fAliasBusy = 0;
            return;
        }
        fAliasTries = 3;
        if (wc && px) gAliasMBps[1] = (int)writeMBps(wc, px, probe);
        // write-back only once that one is gone; the same range, so only written (non-temporal), never read
        if (wc) {
            IOMemoryMap *wb = nullptr;
            OSSafeReleaseNULL(wc);
            if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { wb = dm->map(kIOMapCopybackCache); dm->release(); }
            if (wb && px) gAliasMBps[2] = (int)writeMBps(wb, px, probe);
            if (wb && gAliasMBps[2] > gAliasMBps[1] && gAliasMBps[2] > gAliasMBps[0]) { fAliasMap = wb; gAliasView = 2; }
            else {
                OSSafeReleaseNULL(wb);
                if (gAliasMBps[1] >= gAliasMBps[0] || !px) {
                    if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { fAliasMap = dm->map(kIOMapWriteCombineCache); dm->release(); }
                    if (fAliasMap) gAliasView = 1;
                }
            }
        }
        if (px) IOFree(px, probe);
        static const char *const kName[3] = { "the BAR", "the carve-out, write-combined", "the carve-out, write-back" };
        PFBLOG("alias: writing %llu KB: BAR %d MB/s, carve-out write-combined %d MB/s, write-back %d MB/s -> frames go through %s",
               probe >> 10, gAliasMBps[0], gAliasMBps[1], gAliasMBps[2], kName[gAliasView]);
        fAliasBusy = 0;
    }
    // the view the display pipe writes the next frame into (debug.phxfb_alias 0 = the BAR)
    IOMemoryMap *scanoutView() {
        if (!gAlias) return fSurfMap;
        aliasSetup();
        IOMemoryMap *m = fAliasMap;
        return m ? m : fSurfMap;
    }
    IOService *bringup() {
        if (fBringup || fBringupTries >= 3) return fBringup;
        fBringupTries++;
        if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *e = OSDynamicCast(IOService, o);
                OSNumber *b = e ? OSDynamicCast(OSNumber, e->getProperty("phx-build")) : nullptr;
                if (b && b->unsigned32BitValue() >= 36) { fBringup = e; fBringup->retain(); break; }
                if (b) PFBLOG("PhxBringup build %u has no SDMA scan-out copy (needs 36): frames stay CPU-copied", b->unsigned32BitValue());
            }
            it->release();
        }
        return fBringup;
    }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
            NVRMVramRequest *r = (NVRMVramRequest *)p1;
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
            if (!fSurfMap) return kIOReturnNotReady;
            r->kva = (void *)scanoutView()->getVirtualAddress();
            r->phys = fBase; r->actualSize = round_page_64((uint64_t)fRowBytes * fHeight);
            r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
            return kIOReturnSuccess;
        }
        // p1 the frame's IOMemoryDescriptor, p2 its byte count, p3 PhxAccel's args (PhxBringup 39: 'PSA1', pixels to check, us out)
        if (fn && fn->isEqualTo("phxGpuCopy")) {
            IOService *b = bringup();
            if (!b || fScanBarOff == ~0ull || !p1) return kIOReturnUnsupported;
            if ((uint64_t)(uintptr_t)p2 > (uint64_t)fRowBytes * fHeight) return kIOReturnBadArgument;
            const OSSymbol *sym = OSSymbol::withCStringNoCopy("phxScanoutCopy");
            const IOReturn r = sym ? b->callPlatformFunction(sym, false, p1, p2, (void *)(uintptr_t)fScanBarOff, p3) : kIOReturnNoMemory;
            if (sym) sym->release();
            return r;
        }
        // the firmware's frame back on the panel (the display pipe copies by CPU again)
        if (fn && fn->isEqualTo("phxGpuRelease")) {
            if (!fBringup) return kIOReturnSuccess;
            const OSSymbol *sym = OSSymbol::withCStringNoCopy("phxScanoutRelease");
            if (sym) { fBringup->callPlatformFunction(sym, false, nullptr, nullptr, nullptr, nullptr); sym->release(); }
            return kIOReturnSuccess;
        }
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_ALLOC)) return vramAlloc((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_FREE)) return vramFree((NVRMVramRequest *)p1);
        if (fn && (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES) || fn->isEqualTo(NVRM_GPUVA_FN_ALLOC) || fn->isEqualTo(NVRM_GPUVA_FN_MAP) ||
                   fn->isEqualTo(NVRM_GPUVA_FN_FREE)))
            return rmCall(fn, p1);
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
};

OSDefineMetaClassAndStructors(PhxAMDFB, IOFramebuffer)

// One boot per arming: `nvram phxfb-boot=1` lets the next start take the panel, and start removes the variable and
// commits NVRAM at once, so a boot that panics after this point comes back on the firmware framebuffer without hands.
static bool takeBootArming()
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty("phxfb-boot");
    const bool armed = o != nullptr;
    bool stuck = false;
    OSSafeReleaseNULL(o);
    if (armed) {
        opts->removeProperty("phxfb-boot");
        // the forced commit: the throttled one (IONVRAM-SYNCNOW) left the variable in flash once, and the boot after a
        // panic took the panel again (2026-10-20)
        if (OSString *why = OSString::withCString("phxfb")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
        o = opts->copyProperty("phxfb-boot");
        stuck = o != nullptr;
        PFBLOG("NVRAM phxfb-boot taken (%s)", stuck ? "STILL SET: removal refused, not taking the panel" : "removed and committed");
        OSSafeReleaseNULL(o);
    }
    opts->release();
    return armed && !stuck;
}

bool PhxAMDFB::start(IOService *provider)
{
    char arg[8];
    if (PE_parse_boot_argn("-phxfboff", arg, sizeof arg)) { PFBLOG("-phxfboff: leaving the firmware framebuffer"); return false; }
    if (!takeBootArming()) { PFBLOG("NVRAM phxfb-boot not set: leaving the firmware framebuffer this boot"); return false; }
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject())
            if (o != this && OSDynamicCast(IOFramebuffer, o)) fBeside = true;
        it->release();
    }
    PE_Video v;
    bzero(&v, sizeof(v));
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32 || !v.v_width || !v.v_height ||
        v.v_rowBytes < v.v_width * 4 || v.v_width > 16384 || v.v_height > 16384) {
        PFBLOG("no usable boot console (base 0x%lx %lux%lu depth %lu row %lu): leaving the firmware framebuffer",
               v.v_baseAddr, v.v_width, v.v_height, v.v_depth, v.v_rowBytes);
        return false;
    }
    fWidth = (uint32_t)v.v_width; fHeight = (uint32_t)v.v_height; fRowBytes = (uint32_t)v.v_rowBytes;
    // once the video console is up, the platform expert hands out the console's kernel mapping, not the physical
    // address. The GOP surface sits in one of the GPU's BARs (VRAM aperture): find the BAR whose start holds the same
    // pixels as the console mapping. (A descriptor over the mapping would need prepare(): getPhysicalSegment on an
    // unwired one panicked, 2026-10-20.)
    uint64_t base = v.v_baseAddr;
    if (base >= 0xFFFF800000000000ull) {
        const volatile uint32_t *con = (const volatile uint32_t *)(uintptr_t)base;
        const uint32_t probeWords = 64u * 1024u / 4u;
        uint64_t found = 0;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount() && !found; i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (!dm || dm->getLength() < (IOByteCount)fRowBytes * fHeight) continue;
            IODeviceMemory *sub = IODeviceMemory::withSubRange(dm, 0, 64u * 1024u);
            IOMemoryMap *m = sub ? sub->map(kIOMapInhibitCache) : nullptr;
            OSSafeReleaseNULL(sub);
            if (!m) continue;
            const volatile uint32_t *bar = (const volatile uint32_t *)m->getVirtualAddress();
            uint32_t same = 0, nz = 0;
            for (uint32_t w = 0; w < probeWords; w++) { const uint32_t a = con[w], b = bar[w]; same += a == b; nz += a != 0; }
            PFBLOG("BAR index %u at 0x%llx: %u of %u words match the console (%u non-zero)", i,
                   (unsigned long long)dm->getPhysicalAddress(), same, probeWords, nz);
            if (same == probeWords && nz) found = dm->getPhysicalAddress();
            m->release();
        }
        if (!found) { PFBLOG("the console surface is in none of the GPU's BARs: leaving the firmware framebuffer"); return false; }
        base = found;
    }
    // early in boot the platform expert hands out the physical address with a flag in its low bits (0x820000001): taken
    // as is, every row WindowServer drew landed one byte off and the panel showed swapped colours (2026-10-20)
    base &= ~(uint64_t)PAGE_MASK;
    fBase = (IOPhysicalAddress)base;
    for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
        IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
        if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
            if (i == 0) fScanBarOff = fBase - dm->getPhysicalAddress();   // BAR0 = the carve-out from offset 0
            break;
        }
    }
    PFBLOG("console: %ux%u row %u depth %lu at physical 0x%llx", fWidth, fHeight, fRowBytes, v.v_depth, (unsigned long long)fBase);
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
    }
    if (!fSurfMap) { PFBLOG("the console surface at 0x%llx would not map write-combined", (unsigned long long)fBase); return false; }
    // IOFramebuffer::start makes the framebuffer's controller only from a dependent ID that is already set
    // (IOFBController::copyController(kDepIDCreate)); without one the controller stays NULL and the first power-change
    // callout dereferences it (IOFramebuffer::powerStateWillChangeTo, panic 2026-10-20)
    setProperty("IOFBDependentID", 0x50485846ull << 32 | 1, 64);   // 'PHXF'
    setProperty("IOFBDependentIndex", 0ull, 32);
    if (!IOFramebuffer::start(provider)) { OSSafeReleaseNULL(fSurfMap); return false; }
    fVa = IORangeAllocator::withRange(kVaSize - 1, PAGE_SIZE, 0, IORangeAllocator::kLocking);
    poolInit();
    setProperty("built-in", kOSBooleanTrue);
    setProperty("PhxFBWidth", fWidth, 32); setProperty("PhxFBHeight", fHeight, 32);   // the AGDC's panel timing reads these
    measureRefresh();
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }   // the AGDC's panel timing
    nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);
    fVblTimer = thread_call_allocate(vblFire, this);
    if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
    static IOPMPowerState states[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    registerPowerDriver(this, states, 2);
    phxHoldInit(this, kBootHoldSec);
    phxAgdcInstall();
    if (!gAliasSysctl) { for (auto *o : kFbOids) sysctl_register_oid(o); gAliasSysctl = true; }
    PFBLOG("panel published: %ux%u row %u at 0x%llx (vbl timer %s)", fWidth, fHeight, fRowBytes,
           (unsigned long long)fBase, fVblTimer ? "running" : "MISSING");
    return true;
}

void PhxAMDFB::stop(IOService *provider)
{
    fStopping = true;
    if (fBringup) {   // the firmware's frame back on the panel
        const OSSymbol *sym = OSSymbol::withCStringNoCopy("phxScanoutRelease");
        if (sym) { fBringup->callPlatformFunction(sym, false, nullptr, nullptr, nullptr, nullptr); sym->release(); }
    }
    phxAgdcRemove();
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    if (gAliasSysctl) { for (auto *o : kFbOids) sysctl_unregister_oid(o); gAliasSysctl = false; }
    gPhxFrameAbs = 0;
    OSSafeReleaseNULL(fAliasMap);
    OSSafeReleaseNULL(fBringup);
    OSSafeReleaseNULL(fSurfMap);
    OSSafeReleaseNULL(fVa);
    poolFree();
    IOFramebuffer::stop(provider);
}
