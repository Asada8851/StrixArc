//
//  PhxBringup.cpp — Phoenix (Radeon 780M, 1002:1900) bring-up kext, step E1.
//
//  Publishes the service the RADV Darwin backend opens (class PhxBringup, user client type 'N48N')
//  and answers ABI v1 selectors 0..8 with the same rules as Mesa's in-process mock kext
//  (src/amd/common/darwin/ac_darwin_mock.c): buffers are kernel/user shared system memory,
//  GPU VA mappings are bookkeeping only, and Submit retires at once without touching the GPU.
//  E1 proves the IOKit transport end to end; E3/E4 put the GPU behind the same calls.
//
//  Boot-arg -phxbringupoff disables it. ReadRegs (MMIO read) is root-only; nothing else touches hardware.
//

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <kern/clock.h>

#include "PhxNativeABI.h"
#include "rdna3_chip.h"

// Newer upstream clang (seen with 23.1) lowers the static destructors (the OSMetaClass objects) to a __cxa_atexit
// registration on Darwin even under -mkernel/-fapple-kext, and the kernel exports no __cxa_atexit: such a kext would
// not link at load. Apple's clang puts them in __mod_term_func instead. Keep that behaviour here: record what gets
// registered and run it, newest first, from our own __mod_term_func entry when the kext is unloaded.
extern "C" {
static void (*gAtexitFn[8])(void *);
static void *gAtexitArg[8];
static unsigned gAtexitN;
__attribute__((visibility("hidden"))) int __cxa_atexit(void (*fn)(void *), void *arg, void *) {
    if (gAtexitN >= 8)
        return -1;
    gAtexitFn[gAtexitN] = fn;
    gAtexitArg[gAtexitN++] = arg;
    return 0;
}
static void phxRunAtexit() {
    while (gAtexitN) {
        gAtexitN--;
        gAtexitFn[gAtexitN](gAtexitArg[gAtexitN]);
    }
}
__attribute__((used, section("__DATA,__mod_term_func,mod_term_funcs"))) static void (*const gPhxTerm)() = phxRunAtexit;
}

#define PHX_KEXT_BUILD 40u

// debug.phxbos_flush: how a client's BOs are made coherent around each submission and each completed wait.
// 2 = clflush every line (the bring-up way), 1 = clflushopt (the same lines, pipelined), 0 = none (the GPU maps system
// memory snooped, kPteSysFlags). With clflush, WindowServer spent ~73% of its CPU here and composited ~13 fps; with
// none, 41 fps and no artifacts (2026-10-20), hence the default.
static int gBosFlush = 0;
static volatile SInt64 gBosFlushBytes;
SYSCTL_INT(_debug, OID_AUTO, phxbos_flush, CTLFLAG_RW | CTLFLAG_LOCKED, &gBosFlush, 0,
           "BO cache maintenance per submit/wait: 0 none, 1 clflushopt, 2 clflush");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushBytes,
            "bytes of BOs flushed from the CPU caches");
// Scan-out copies by SDMA (build 36): debug.phxscanout_verify checks 32 pixels of each copied frame against its source
// before the flip (a mismatch puts the firmware's frame back and turns GPU copies off until reboot).
static int gScanoutVerify = 1;
SYSCTL_INT(_debug, OID_AUTO, phxscanout_verify, CTLFLAG_RW | CTLFLAG_LOCKED, &gScanoutVerify, 0,
           "1 = check 32 pixels of every SDMA-copied frame before it is flipped to");
static SInt64 gScanoutCopies, gScanoutUsSum, gScanoutBusy, gScanoutMismatch, gScanoutMoving, gScanoutGpuWaitUs, gScanoutGpuLate;
static char gScanoutLastBad[96] = "none";
static char gScanoutState[96] = "not used yet";
SYSCTL_STRING(_debug, OID_AUTO, phxscanout_state, CTLFLAG_RD | CTLFLAG_LOCKED, gScanoutState, sizeof(gScanoutState),
              "scan-out copies: on, or why they are off");
class PhxBringup;
static PhxBringup *gBringup;
static int gScanoutSelftestRes;   // 0 not run, 1 passed, 2 wrong data, 3 timed out, 4 not set up
static int phxscanout_selftest_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxscanout_selftest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
            phxscanout_selftest_sysctl, "I",
            "write 1: SDMA copies 1 MiB of system RAM into the carve-out; reads 1 passed, 2 wrong data, 3 timed out, 4 not set up");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_copies, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutCopies, "frames SDMA copied and the display flipped to");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_us_sum, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutUsSum, "microseconds those copies took, call to flip");
// debug.phxdmcub (read, binary: sysctl -b): what the firmware left DMCUB, for 120 Hz (a link retrain at HBR3 goes through
// DMCUB's VBIOS command tables, as Linux's DC does it). Read-only: 'DMCB', the DMU register block 0x3580..0x36ff, and
// 1 MiB of the carve-out from DMCUB's region 4 (its inbox) on, which holds the windows of its firmware, trace buffer and
// state (the GOP left them at the top of VRAM).
static int phxdmcub_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxdmcub, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxdmcub_sysctl, "S",
            "DMCUB snapshot: 'DMCB', DMU registers 0x3580..0x36ff, 1 MiB of its carve-out windows");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_moving, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutMoving,
            "frames dropped because the source still changed after the copy (the GPU still drawing it)");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_gpuwait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutGpuWaitUs,
            "microseconds frames waited for the GPU work submitted before them, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_gpulate, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutGpuLate,
            "frames copied although the GPU work before them had not finished in 50 ms");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_mismatch, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutMismatch,
            "frames dropped because a checked pixel differed from the source (8 in a row turn GPU copies off)");
SYSCTL_STRING(_debug, OID_AUTO, phxscanout_lastbad, CTLFLAG_RD | CTLFLAG_LOCKED, gScanoutLastBad, sizeof(gScanoutLastBad),
              "the last pixel that differed: offset, copied, source");
SYSCTL_QUAD(_debug, OID_AUTO, phxscanout_busy, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutBusy, "frames skipped: the previous flip had not landed yet");
static volatile UInt32 gSysctlUp;

// psp_gfx_rb_frame (64 B) — the ring entry the PSP reads. Only 5 fields are set; the rest stay zero
// (Linux amdgpu_psp.c:3485: a non-zero cmd_buf_size makes the PSP silently drop the frame).
struct PhxGfxRbFrame {
    uint32_t cmd_buf_addr_lo, cmd_buf_addr_hi, cmd_buf_size;
    uint32_t fence_addr_lo, fence_addr_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PhxGfxRbFrame) == 64, "PhxGfxRbFrame must be 64 B");

// GB_ADDR_CONFIG and the GC version come from the chip profile (src/amd/rdna3_chip.h): Mesa's canned "phoenix" or
// "strix1" table values.

// Pools reported to RADV. E1 backs every BO with system memory, so these are budgets, not hardware:
// BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB.
static constexpr uint64_t kVisTotal = 256ull << 20;
static constexpr uint64_t kHiTotal = 1536ull << 20;
static constexpr uint64_t kGttCap = 512ull << 20;
static constexpr uint64_t kGttMaxBo = 256ull << 20;
static constexpr uint64_t kHostCap = 4096ull << 20, kHostMaxBo = 1024ull << 20;   // host imports, per client
static constexpr uint64_t kVaMask = (1ull << 48) - 1;
static constexpr int kMaxMaps = 4096;

static bool vaCanonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

class PhxBringup : public IOService {
    OSDeclareDefaultStructors(PhxBringup)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;

    bool claimSession();
    void releaseSession();
    IOReturn readRegs(uint32_t first, uint32_t count, uint32_t *out);
    IOReturn vramRead(uint32_t byteOff, uint32_t count, uint32_t *out);
    IOReturn writeReg(uint32_t reg, uint32_t value, uint32_t *readback);
    IOReturn vramWrite(uint32_t byteOff, uint32_t value);
    IOReturn doorbellWrite(uint64_t byteOff, uint64_t value);
    IOReturn sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1);
    IOReturn smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);
    IOReturn pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]);
    IOReturn pspSubmit(uint32_t cmdId, uint64_t out[4]);
    IOReturn fwStage(uint32_t dstOffset, const void *bytes, uint32_t len);
    IOReturn loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]);
    IOReturn pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]);
    IOReturn pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]);
    bool debugEnabled() const { return debug; }
    const rdna3_chip *chipProfile() const { return chip; }
    // E8: GPU execution. The userspace bring-up maps MES queues (compute = 0, gfx = 1) and hands them over (adoptQueue);
    // user BOs are then mapped into the VMID 8 page table and SUBMIT writes INDIRECT_BUFFERs into the queue's ring.
    IOReturn adoptQueue(const uint64_t in[6]);
    IOReturn claimAccel();
    bool gpuReady() const { return gq[0].ready || gq[1].ready; }
    // one GPUVM context (VMID 8..15) per client process: RADV instances allocate VAs independently
    uint32_t vmidAlloc();
    void vmidFree(uint32_t vmid, uint64_t lastSeq);
    IOReturn vmMap(uint32_t vmid, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size);
    IOReturn vmUnmap(uint32_t vmid, uint64_t va, uint64_t size);
    IOReturn gpuSubmit(uint32_t vmid, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seq);
    uint64_t gpuRetired() const;
    bool gpuSeqDone(uint64_t seq) const;
    // Build 36: a finished frame goes to the panel without the CPU (PhxAMDFB calls "phxScanoutCopy": p1 the frame's
    // IOMemoryDescriptor, p2 its byte count, p3 the firmware frame's carve-out offset, p4 uint64_t * for the
    // microseconds it took). Under macOS the display cannot fetch system RAM (DCN 3.5 sends it to an rIOMMU nobody
    // turns on), but SDMA reaches both: one COPY_LINEAR into one of two buffers in the carve-out, then pipe 0 flips to it
    // at the next vblank. Returns kIOReturnBusy when the last flip has not landed (the frame is skipped); any other
    // failure puts the firmware's frame back on the panel first, so the caller's CPU copy is what shows.
    // phxScanoutCopy's 4th parameter from PhxAccel 40+ ('PSA2'): the caller's own kernel mapping of the frame, read here
    // after the copy (cache lines flushed first) to check 32 pixels. Build 38 mapped the frame in the kernel from inside
    // the display pipe's transaction and the boot hung (2026-10-26 22:11): no mapping made here. 'PSA1' (PhxAccel 39:
    // pixels sampled before the copy, racing the GPU still drawing the frame - Launchpad's blur, 23:11) and a bare
    // uint64_t * for the microseconds (older) get no check.
    struct ScanArgs {
        uint32_t magic, n;   // 'PSA2', samples (up to 32)
        uint64_t us;         // out: microseconds the copy took
        uint64_t srcKva;     // the frame's first byte, in the caller's mapping (valid during the call)
        uint32_t off[32];
    };
    IOReturn scanoutCopy(IOMemoryDescriptor *src, uint64_t bytes, uint64_t gopOff, const ScanArgs *chk, uint64_t *usOut);
    void scanoutRelease();   // the firmware's frame back on the panel (the caller copies by CPU from now on)
    // debug.phxscanout_selftest: SDMA copies 1 MiB of system RAM into the carve-out, checked by the CPU. Returns
    // 1 passed, 2 wrong data, 3 timed out (GPU copies off until reboot), 4 could not be set up.
    int scanoutSelftest();
    int dmcubDump(struct sysctl_req *req);
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                  void *param3, void *param4) override;

private:
    uint32_t rreg(uint32_t reg) const;
    void wreg(uint32_t reg, uint32_t value) const;
    uint32_t smuPoll() const;
    bool pspWaitTos(uint32_t timeoutMs) const;
    IOReturn pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]);
    void hdpFlush() const;
    void readFbLayout();
    void clearRing();

    IOPCIDevice *pci{nullptr};
    const rdna3_chip *chip{nullptr};   // set in start() from the PCI device ID
    IOMemoryMap *mmio{nullptr};
    IOLock *smuLock{nullptr};
    // UMA carve-out ("VRAM"): GPU MC address range and the system physical address behind it.
    uint64_t fbMcBase{0}, fbSize{0}, fbPhys{0};
    // BAR0 framebuffer aperture: the CPU window into the first fbAperLen bytes of VRAM. It is an MMIO
    // region above system RAM (phys 0x8_2040_0000 here, 256 MiB), so CPU writes through it reach the
    // GPU's local memory and never the macOS heap. This is how the PSP ring is initialised.
    IOMemoryMap *fbAper{nullptr};
    IOMemoryMap *doorbellMap{nullptr}; // BAR2: doorbell aperture (2 MiB)
    IOBufferMemoryDescriptor *sysBuf{nullptr}; // debug: physically contiguous system memory for GPU VM experiments
    uint64_t fbAperPhys{0}, fbAperLen{0};
    // MMIO dword to write 0 to for an HDP flush (0 if the NBIO remap register was implausible).
    uint32_t hdpFlushDword{0};
    // PSP KM ring (debug selector 101): MC address once created, else 0. The GPCOM command + fence buffers
    // (selector 103) live just above the ring in VRAM; offsets are VRAM byte offsets, Mc the matching MC address.
    uint64_t ringMc{0}, ringOff{0};
    uint64_t cmdOff{0}, cmdMc{0}, fenceOff{0}, fenceMc{0};
    uint32_t fenceCounter{0};
    // Firmware bring-up regions (set when the ring is created): TMR the PSP copies firmware into, and fw_buf
    // where the CPU stages ucode for LOAD_IP_FW. Offsets are VRAM byte offsets, Mc the matching MC addresses.
    uint64_t tmrOff{0}, tmrMc{0}, tmrSize{0};
    uint64_t fwBufOff{0}, fwBufMc{0}, fwBufSize{0};
    bool tmrSetUp{false};
    // E8 state: the adopted queues (ring/wb/fence are VRAM byte offsets reached through BAR0) and the VMID 8
    // page-table pool (physically contiguous system memory; page 0 is the root). Sequence numbers are global across
    // the queues; each queue's fence holds the last sequence it finished, and fseq/fend keep its outstanding
    // submissions in order (sequence, ring position after it).
    struct GpuQueue {
        bool ready, gfx;
        uint32_t ringOff, ringDw, wbOff, fenceOff;
        uint64_t dbByte, wptr, doneEnd;
        uint64_t fseq[64], fend[64];
        volatile uint32_t head, tail;
    };
    GpuQueue gq[2]{};
    uint64_t gSeq{0};
    uint64_t queueFence(const GpuQueue &q) const;
    IOLock *gpuLock{nullptr};
    // One GPUVM context (page tables) per client, and the eight hardware VMIDs lent to contexts at submission time,
    // as amdgpu does: a fixed VMID per client capped Metal at eight processes, and with every app allowed Metal the
    // ninth got no device (2026-10-20). A context's handle is its index + 1.
    // A context's page tables, in physically contiguous 64 KiB chunks allocated as it maps (most contexts need one or
    // two): a single 4 MiB contiguous pool per context failed once memory was fragmented. Table i is page i % 16 of
    // chunk i / 16; table 0 is the root.
    static constexpr uint32_t kPtChunkBytes = 64u << 10, kPtChunkPages = kPtChunkBytes / 4096, kPtChunks = 64;
    struct VmCtx {
        IOBufferMemoryDescriptor *chunk[kPtChunks];
        uint64_t chunkPhys[kPtChunks];
        uint64_t phys;                    // the root table
        uint32_t pages, used;
        bool busy;
        uint32_t hw;                      // the hardware VMID pointed at these tables, 0 = none
    };
    static constexpr uint32_t kMaxCtx = 96;
    VmCtx vm[kMaxCtx]{};
    struct HwVmid {
        uint32_t ctx;                     // handle of the context it points at, 0 = free
        uint64_t lastSeq, lastUse;        // its newest submission; when it was last bound or used
    };
    HwVmid hwv[8]{};
    uint64_t hwTick{0};
    uint32_t vmBind(uint32_t handle);
    VmCtx *vmCtx(uint32_t handle) { return handle && handle <= kMaxCtx && vm[handle - 1].busy ? &vm[handle - 1] : nullptr; }
    uint64_t *ptTable(const VmCtx &c, uint64_t phys) const;
    uint64_t ptTablePhys(VmCtx &c, uint32_t i);
    uint64_t *vmPte(VmCtx &c, uint64_t va, bool alloc);
    void vmFlushTlb(uint32_t vmid) const;
    void ringPut(GpuQueue &q, uint32_t idx, uint32_t value);
    // Scan-out copies (build 36): SDMA0 queue 0 (its ring, write-back and IB in the carve-out), VMID 7 (outside the 8..15
    // lent to clients) over a page-table context of its own that maps the IB, the two scan-out buffers and the frames
    // (WindowServer rotates two or three display surfaces; each stays mapped, and retained, until a newer one takes its
    // slot). mesboot sdmatest proved every piece on the 880M: 28 GB/s, a 3072x1920 frame in 0.84 ms (2026-10-26).
    static constexpr uint64_t kBlitSrcVa = 0x100000000ull, kBlitSlotSpan = 128ull << 20;
    static constexpr uint32_t kBlitSlots = 4;
    struct BlitSrc {
        IOMemoryDescriptor *md;
        uint64_t len, lastUse;
    };
    IOLock *blitLock{nullptr};
    uint32_t blitCtx{0};
    bool sdmaUp{false};
    const char *blitBroken{nullptr};      // why GPU copies are off until reboot; nullptr while they are on
    uint32_t sdWp{0}, sdSeq{0}, badRun{0};
    int scanFront{-1};                    // the buffer pipe 0 was last flipped to; -1 = the firmware's frame
    uint32_t gopHi{0}, gopLo{0};          // pipe 0's surface address before the first flip
    uint64_t gopOff{~0ull}, scanBytes{0}, blitTick{0};
    BlitSrc blitSrc[kBlitSlots]{};
    void blitReleaseAll();
    bool blitSetup();
    bool scanSetup(uint64_t bytes, uint64_t gopOffIn);
    bool sdmaStart();
    void sdmaStop(bool reset);
    void sdPut(uint32_t v);
    void sdKick();
    IOReturn sdRun(uint32_t ibDw, uint32_t timeoutMs);
    void scanToGop();
    IOReturn blitFail(const char *why, IOReturn r);
    IOReturn vmMapLocal(uint32_t handle, uint64_t va, uint64_t phys, uint64_t size);
    volatile uint32_t &vram32(uint64_t off) const {
        return *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + off);
    }
    bool debug{false};
    bool smuUsed{false};
    volatile SInt32 sessionOpen{0};
};

OSDefineMetaClassAndStructors(PhxBringup, IOService)

class IOAccelPhxNativeClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelPhxNativeClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    struct Bo {
        IOMemoryDescriptor *md;   // an IOBufferMemoryDescriptor, or the caller's own pages (host, prepared)
        uint64_t size;
        uint32_t domain;
        uint32_t bits;
        uint8_t *kva;             // the kernel's view of the BO; NULL for host imports, which the kernel never reads
        bool host;
    };
    struct Map {
        bool used;
        uint32_t handle;
        uint64_t va, size, off;
        uint32_t flags;
    };

    IOReturn doHello(IOExternalMethodArguments *a);
    IOReturn doQueryInfo(IOExternalMethodArguments *a);
    IOReturn doReadRegs(IOExternalMethodArguments *a);
    IOReturn doBoCreate(IOExternalMethodArguments *a);
    IOReturn doBoFree(IOExternalMethodArguments *a);
    IOReturn doBoImportHost(IOExternalMethodArguments *a);
    void boRelease(Bo &b);
    IOReturn doGemVa(IOExternalMethodArguments *a);
    IOReturn doCtx(IOExternalMethodArguments *a);
    IOReturn doSubmit(IOExternalMethodArguments *a);
    IOReturn doWaitSeq(IOExternalMethodArguments *a);
    IOReturn doDbgSmu(IOExternalMethodArguments *a);
    IOReturn doDbgPspRing(IOExternalMethodArguments *a);
    IOReturn doDbgVramRead(IOExternalMethodArguments *a);
    IOReturn doDbgPspSubmit(IOExternalMethodArguments *a);
    IOReturn doDbgFwStage(IOExternalMethodArguments *a);
    IOReturn doDbgLoadIpFw(IOExternalMethodArguments *a);
    IOReturn doDbgPspCtl(IOExternalMethodArguments *a);
    IOReturn doDbgPspRaw(IOExternalMethodArguments *a);
    IOReturn doDbgWriteReg(IOExternalMethodArguments *a);
    IOReturn doDbgVramWrite(IOExternalMethodArguments *a);
    IOReturn doDbgDoorbell(IOExternalMethodArguments *a);
    IOReturn doDbgSysMem(IOExternalMethodArguments *a);
    IOReturn doDbgAdoptQueue(IOExternalMethodArguments *a);
    IOReturn doDbgClaim(IOExternalMethodArguments *a);
    void flushBos();
    Bo *findBo(uint32_t h);
    bool ibCovered(uint64_t vaCanon, uint64_t bytes);
    void freeAll();

    PhxBringup *owner{nullptr};
    IOLock *lock{nullptr};
    bool privileged{false};
    bool hello{false};
    bool sessionClaimed{false};
    uint32_t vmid{0};        // this client's GPUVM context, taken on its first GPU mapping or submission
    uint64_t lastSeq{0};     // its newest submission: the context is released only once the GPU is past it
    bool ensureVmid() { return vmid || (vmid = owner->vmidAlloc()) != 0; }
    Bo *bos{nullptr};      // PHXN_MAX_BOS entries, index = handle
    Map *maps{nullptr};    // kMaxMaps entries
    bool ctx[PHXN_MAX_CTX + 1]{};
    uint64_t visUsed{0}, hiUsed{0}, gttUsed{0}, hostUsed{0};
    task_t task{nullptr};
    uint64_t emitted{0};
    uint32_t pendFenceHandle{0}, pendFenceOffset{0};
    uint64_t pendFenceSeq{0};
};

OSDefineMetaClassAndStructors(IOAccelPhxNativeClient, IOUserClient)

// ---- service ---------------------------------------------------------------------------------------------

bool PhxBringup::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-phxbringupoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;
    {
        const uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
        chip = rdna3_chip_for_pci(dev);
        if (!chip) {
            IOLog("PhxBringup: GPU 1002:%04x is not one nullarc knows\n", dev);
            pci = nullptr;
            return false;
        }
        // A chip whose profile never ran on hardware (Strix Point) only starts with -phxexperimental: a wrong
        // register there can hang the machine at boot, and phxboot runs the bring-up at every boot.
        if (!chip->verified_on_hardware && !PE_parse_boot_argn("-phxexperimental", &dummy, sizeof(dummy))) {
            IOLog("PhxBringup: %s support is untested; boot with -phxexperimental to load anyway\n", chip->name);
            pci = nullptr;
            return false;
        }
    }
    pci->retain();
    {   // the GPU must be a bus master before it can DMA to/from system memory (page tables, user BOs)
        uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
        setProperty("phx-pci-cmd-before", cmd, 16);
        pci->setBusMasterEnable(true);
        pci->setMemoryEnable(true);
        setProperty("phx-pci-cmd-after", pci->configRead16(kIOPCIConfigCommand), 16);
    }
    mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    fbAper = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fbAper) {
        fbAperLen = fbAper->getLength();
        fbAperPhys = fbAper->getPhysicalAddress();
    }
    doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    smuLock = IOLockAlloc();
    gpuLock = IOLockAlloc();
    blitLock = IOLockAlloc();
    uint32_t dbg = 0;
    debug = PE_parse_boot_argn("phxdebug", &dbg, sizeof(dbg)) && dbg == 1;
    if (mmio) {
        readFbLayout();
        // HDP flush window: the NBIO remap register holds the BAR5 byte offset where writing 0 flushes the
        // host data path. Accept it only if it is a plausible dword offset inside our MMIO aperture.
        // (0 in the profile = not known for this chip; a register beyond BAR5 would need indirect access)
        const uint32_t remapReg = chip->nbio_hdp_remap;
        uint32_t remap = remapReg && (uint64_t)remapReg * 4 + 4 <= mmio->getLength() ? rreg(remapReg) : 0;
        if (remap && remap != 0xFFFFFFFFu && (remap & 3) == 0 && remap + 4 <= mmio->getLength())
            hdpFlushDword = remap >> 2;
        // Strix Point: the remap register sits beyond BAR5 (NBIO 7.11 segment 5), so flush through the HDP flush
        // register itself, the one Linux's NBIO 7.11 code writes (BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL)
        const char *hdpSrc = hdpFlushDword ? "remap" : "none";
        if (!hdpFlushDword && chip->hdp_flush_direct &&
            (uint64_t)chip->hdp_flush_direct * 4 + 4 <= mmio->getLength()) {
            hdpFlushDword = chip->hdp_flush_direct;
            hdpSrc = "direct";
        }
        setProperty("phx-hdp-flush-dword", hdpFlushDword, 32);
        setProperty("phx-hdp-flush-src", hdpSrc);
    }
    setProperty("phx-build", PHX_KEXT_BUILD, 32);
    setProperty("phx-chip", chip->name);
    setProperty("phx-scanout-copy", "sdma");
    {   // how the firmware typed the carve-out for the CPU: CPU writes into it ran at ~55 MB/s under macOS (uncached
        // speed), with no variable MTRR above 4 GiB under Linux; TOM2/SYSCFG decide whether memory above 4 GiB is WB
        auto rdmsr = [](uint32_t msr) {
            uint32_t lo, hi;
            __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
            return (static_cast<uint64_t>(hi) << 32) | lo;
        };
        setProperty("phx-msr-tom", rdmsr(0xC001001Au), 64);
        setProperty("phx-msr-tom2", rdmsr(0xC001001Du), 64);
        setProperty("phx-msr-syscfg", rdmsr(0xC0010010u), 64);
        setProperty("phx-msr-mtrr-deftype", rdmsr(0x2FFu), 64);
        setProperty("phx-msr-pat", rdmsr(0x277u), 64);
        const uint32_t nVar = static_cast<uint32_t>(rdmsr(0xFEu) & 0xFF);   // MTRRcap.VCNT
        uint64_t var[32];
        uint32_t n = 0;
        for (uint32_t i = 0; i < nVar && i < 16; i++) {
            var[n++] = rdmsr(0x200u + 2 * i);
            var[n++] = rdmsr(0x201u + 2 * i);
        }
        if (OSData *d = OSData::withBytes(var, n * 8)) {   // base, mask pairs
            setProperty("phx-msr-mtrr-var", d);
            d->release();
        }
    }
    gBringup = this;
    setProperty("phx-stage", "E3");
    setProperty("phx-debug", debug);
    setProperty("phx-fb-aper-phys", fbAperPhys, 64);
    setProperty("phx-fb-aper-len", fbAperLen, 64);
    IOLog("PhxBringup: service up (mmio %s, bar0 %s %llu MiB, debug %d)\n", mmio ? "mapped" : "unmapped",
          fbAper ? "mapped" : "unmapped", (unsigned long long)(fbAperLen >> 20), debug);
    registerService();
    return true;
}

void PhxBringup::stop(IOService *provider) {
    if (gSysctlUp) {
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush);
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_verify);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_state);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_copies);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_us_sum);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_busy);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_mismatch);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_moving);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_gpuwait_us);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_gpulate);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_lastbad);
        sysctl_unregister_oid(&sysctl__debug_phxdmcub);
        sysctl_unregister_oid(&sysctl__debug_phxscanout_selftest);
        gSysctlUp = 0;
    }
    gBringup = nullptr;
    if (blitLock) {   // SDMA stopped and the firmware's frame back on the panel while the BAR and MMIO are still mapped
        IOLockLock(blitLock);
        blitReleaseAll();
        IOLockUnlock(blitLock);
    }
    clearRing();
    if (fbAper) {
        fbAper->release();
        fbAper = nullptr;
    }
    if (sysBuf) {
        sysBuf->release();
        sysBuf = nullptr;
    }
    if (blitLock) {
        IOLockFree(blitLock);
        blitLock = nullptr;
    }
    gq[0].ready = gq[1].ready = false;
    for (VmCtx &c : vm) {
        for (IOBufferMemoryDescriptor *b : c.chunk)
            if (b)
                b->release();
        c = VmCtx{};
    }
    for (HwVmid &h : hwv)
        h = HwVmid{};
    if (gpuLock) {
        IOLockFree(gpuLock);
        gpuLock = nullptr;
    }
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
    }
    if (mmio) {
        mmio->release();
        mmio = nullptr;
    }
    if (pci) {
        pci->release();
        pci = nullptr;
    }
    if (smuLock) {
        IOLockFree(smuLock);
        smuLock = nullptr;
    }
    IOService::stop(provider);
}

uint32_t PhxBringup::rreg(uint32_t reg) const {
    return reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg];
}

void PhxBringup::wreg(uint32_t reg, uint32_t value) const {
    reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg] = value;
}

// FB location (MC, 16 MiB units, FB_BASE bits 23:0) and the system physical address of the carve-out (FB_OFFSET << 24).
void PhxBringup::readFbLayout() {
    // MMHUB 3.0.1 on Phoenix, 3.3.x on Strix Point: the same registers 0x100 dwords apart (src/amd/rdna3_chip.h)
    uint32_t base = rreg(chip->mmhub_fb_location_base) & 0x00FFFFFF;
    uint32_t top = rreg(chip->mmhub_fb_location_top) & 0x00FFFFFF;
    fbMcBase = static_cast<uint64_t>(base) << 24;
    fbSize = top >= base ? (static_cast<uint64_t>(top - base) + 1) << 24 : 0;
    fbPhys = static_cast<uint64_t>(rreg(chip->mmhub_fb_offset)) << 24;
    setProperty("phx-fb-mc-base", fbMcBase, 64);
    setProperty("phx-fb-size", fbSize, 64);
    setProperty("phx-fb-phys", fbPhys, 64);
    // About This Mac / System Information name the GPU from the PCI device's "model" and size its memory from
    // "VRAM,totalMB"; without them the 780M read "Display, 15 MB" (the boot framebuffer)
    if (OSData *m = OSData::withBytes(chip->model, static_cast<unsigned>(strlen(chip->model) + 1))) {
        pci->setProperty("model", m);
        m->release();
    }
    if (fbSize)
        pci->setProperty("VRAM,totalMB", fbSize >> 20, 32);
}

void PhxBringup::clearRing() {
    ringMc = ringOff = 0;
    cmdOff = cmdMc = fenceOff = fenceMc = 0;
    fenceCounter = 0;
    tmrOff = tmrMc = tmrSize = 0;
    fwBufOff = fwBufMc = fwBufSize = 0;
    tmrSetUp = false;
}

// Drain the host data path so the PSP sees our VRAM writes before we ring the doorbell (C2PMSG_67).
void PhxBringup::hdpFlush() const {
    if (hdpFlushDword)
        wreg(hdpFlushDword, 0);
    (void)rreg(0x0DE3);   // read-back barrier (GRBM_STATUS), as in the reference
}

// Read-only: count dwords from VRAM at byteOff, through the BAR0 aperture. Used by debug selector 102 to
// find the live console framebuffer and a free (all-zero) VRAM region for the PSP structures.
IOReturn PhxBringup::vramRead(uint32_t byteOff, uint32_t count, uint32_t *out) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || (static_cast<uint64_t>(byteOff) + static_cast<uint64_t>(count) * 4) > fbAperLen)
        return kIOReturnBadArgument;
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff);
    for (uint32_t i = 0; i < count; i++)
        out[i] = v[i];
    return kIOReturnSuccess;
}

// Write one MMIO dword (bring-up experimentation; root + phxdebug only). Returns the readback.
IOReturn PhxBringup::writeReg(uint32_t reg, uint32_t value, uint32_t *readback) {
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    wreg(reg, value);
    __asm__ __volatile__("mfence" ::: "memory");
    *readback = rreg(reg);
    return kIOReturnSuccess;
}

// Write one dword into VRAM at byteOff through the BAR0 aperture.
// op 1: allocate a physically contiguous buffer of a bytes (replaces any previous), out0 = physical address
// op 2: read dword at byte offset a -> out1;  op 3: write dword b at byte offset a;  op 4: free
IOReturn PhxBringup::sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1) {
    *out0 = *out1 = 0;
    if (op == 1) {
        if (sysBuf) {
            sysBuf->release();
            sysBuf = nullptr;
        }
        if (a == 0 || a > (64ull << 20))
            return kIOReturnBadArgument;
        sysBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, a, 0x00007FFFFFFFF000ull);
        if (!sysBuf)
            return kIOReturnNoMemory;
        bzero(sysBuf->getBytesNoCopy(), a);
        *out0 = sysBuf->getPhysicalAddress();
        *out1 = sysBuf->getLength();
        return kIOReturnSuccess;
    }
    if (!sysBuf)
        return kIOReturnNotReady;
    if (op == 4) {
        sysBuf->release();
        sysBuf = nullptr;
        return kIOReturnSuccess;
    }
    if ((a & 3) || a + 4 > sysBuf->getLength())
        return kIOReturnBadArgument;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(sysBuf->getBytesNoCopy()) + a);
    if (op == 2) {
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
        *out1 = *p;
        *out0 = sysBuf->getPhysicalAddress();
        return kIOReturnSuccess;
    }
    if (op == 3) {
        *p = static_cast<uint32_t>(b);
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); // GPU walker may not snoop CPU caches
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

// ---- E8: GPU execution ------------------------------------------------------------------------------------
// Everything here reproduces what tools/phxctl/mesboot.c proved on the 780M: VMID 8 with a 4-level table in
// system memory (PDE = next|VALID|SYSTEM|SNOOPED, no BFS/TF since gmc_v11 has translate_further off), system-page
// PTEs with MTYPE 4, invalidation on engine 17, and an ACQUIRE/RELEASE_MEM GL2 writeback after each IB.
static constexpr uint32_t kFirstUserVmid = 8, kNumUserVmids = 8;   // VMIDs 8..15, lent to client contexts (vmBind)
static constexpr uint64_t kPteSysFlags = 0x77ull | (4ull << 48);  // VALID SYSTEM SNOOPED EXEC READ WRITE, MTYPE 4
static constexpr uint64_t kPtAddrMask = 0x0000FFFFFFFFF000ull;
// GC registers from the chip profile (absolute dword index): context n = context 0 + n (CNTL) / + 2n (address pairs);
// TLB invalidations go through engine 17, the one amdgpu uses for GMC flushes.
static constexpr uint32_t kInvEng = 17;

static inline void flushLine(const volatile void *p) {
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
}

uint64_t *PhxBringup::ptTable(const VmCtx &c, uint64_t phys) const {
    for (uint32_t i = 0; i < kPtChunks && c.chunk[i]; i++)
        if (phys - c.chunkPhys[i] < kPtChunkBytes)
            return reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(c.chunk[i]->getBytesNoCopy()) + (phys - c.chunkPhys[i]));
    return nullptr;
}

// The physical address of table i, allocating (zeroed, flushed) the chunk it lives in; 0 when that fails.
uint64_t PhxBringup::ptTablePhys(VmCtx &c, uint32_t i) {
    const uint32_t k = i / kPtChunkPages;
    if (k >= kPtChunks)
        return 0;
    if (!c.chunk[k]) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kPtChunkBytes, kPtAddrMask);
        if (!b)
            return 0;
        uint8_t *p = static_cast<uint8_t *>(b->getBytesNoCopy());
        bzero(p, kPtChunkBytes);
        for (uint32_t o = 0; o < kPtChunkBytes; o += 64)
            flushLine(p + o);
        c.chunk[k] = b;
        c.chunkPhys[k] = b->getPhysicalAddress();
    }
    return c.chunkPhys[k] + static_cast<uint64_t>(i % kPtChunkPages) * 4096;
}

uint64_t *PhxBringup::vmPte(VmCtx &c, uint64_t va, bool alloc) {
    uint64_t tbl = c.phys;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = ptTable(c, tbl);
        if (!t)
            return nullptr;
        uint64_t *e = t + ((va >> shift) & 511);
        if (!(*e & 1)) {
            if (!alloc || c.used >= c.pages)
                return nullptr;
            const uint64_t child = ptTablePhys(c, c.used);
            if (!child)
                return nullptr;
            c.used++;
            *e = child | 7;
            flushLine(e);
        }
        tbl = *e & kPtAddrMask;
    }
    uint64_t *t = ptTable(c, tbl);
    return t ? t + ((va >> 12) & 511) : nullptr;
}

void PhxBringup::vmFlushTlb(uint32_t vmid) const {
    __asm__ __volatile__("mfence" ::: "memory");
    wreg(chip->gcvm.inv_eng0_req + kInvEng, 0x00F80000u | (1u << vmid) | 1u);
    for (int i = 0; i < 1000 && !(rreg(chip->gcvm.inv_eng0_ack + kInvEng) & (1u << vmid)); i++)
        IODelay(1);
}

// Takes a free context: empty tables (root only), no hardware VMID until its first submission. Returns its handle,
// 0 when all kMaxCtx are in use.
uint32_t PhxBringup::vmidAlloc() {
    if (!gpuReady())
        return 0;
    IOLockLock(gpuLock);
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        VmCtx &c = vm[i];
        if (c.busy)
            continue;
        if (!c.chunk[0]) {
            c.phys = ptTablePhys(c, 0);
            if (!c.phys)
                break;
            c.pages = kPtChunks * kPtChunkPages;
        }
        c.used = 1;
        c.busy = true;
        c.hw = 0;
        IOLockUnlock(gpuLock);
        return i + 1;
    }
    IOLockUnlock(gpuLock);
    return 0;
}

// Points a hardware VMID at the context's tables for a submission (gpuLock held): the one it already has, a free one,
// or the least recently used one whose submissions the GPU has all finished (waits for one, bounded ~2 s). 0 on timeout.
uint32_t PhxBringup::vmBind(uint32_t handle) {
    VmCtx &c = vm[handle - 1];
    const uint64_t now = ++hwTick;
    if (c.hw) {
        hwv[c.hw - kFirstUserVmid].lastUse = now;
        return c.hw;
    }
    int pick = -1;
    for (int spin = 0; pick < 0; spin++) {
        const uint64_t done = gpuRetired();
        uint64_t oldest = ~0ull;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {
            if (!hwv[i].ctx) {
                pick = static_cast<int>(i);
                break;
            }
            if (hwv[i].lastSeq <= done && hwv[i].lastUse < oldest) {
                oldest = hwv[i].lastUse;
                pick = static_cast<int>(i);
            }
        }
        if (pick >= 0)
            break;
        if (spin > 40000)
            return 0;
        IODelay(50);
    }
    HwVmid &h = hwv[pick];
    if (h.ctx)
        vm[h.ctx - 1].hw = 0;
    h.ctx = handle;
    h.lastSeq = 0;
    h.lastUse = now;
    const uint32_t vmid = kFirstUserVmid + static_cast<uint32_t>(pick);
    c.hw = vmid;
    const rdna3_gcvm_regs &g = chip->gcvm;
    wreg(g.ctx0_pt_base_lo + 2 * vmid, static_cast<uint32_t>(c.phys | 3));
    wreg(g.ctx0_pt_base_lo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
    wreg(g.ctx0_cntl + vmid, 0x01fffe07);
    wreg(g.ctx0_pt_start_lo + 2 * vmid, 0);
    wreg(g.ctx0_pt_start_lo + 2 * vmid + 1, 0);
    wreg(g.ctx0_pt_end_lo + 2 * vmid, 0xffffffff);
    wreg(g.ctx0_pt_end_lo + 2 * vmid + 1, 0xf);
    vmFlushTlb(vmid);
    return vmid;
}

// Returns a context once the GPU is past the client's last submission (bounded wait): the tables used so far are
// cleared, so nothing the client mapped stays reachable, and its hardware VMID (if any) is freed.
void PhxBringup::vmidFree(uint32_t handle, uint64_t lastSeq) {
    if (!vmCtx(handle))
        return;
    for (int i = 0; i < 2000 && gpuRetired() < lastSeq; i++)
        IOSleep(1);
    IOLockLock(gpuLock);
    VmCtx &c = vm[handle - 1];
    for (uint32_t k = 0; k < kPtChunks && c.chunk[k] && k * kPtChunkPages < c.used; k++) {
        const uint32_t n = c.used - k * kPtChunkPages < kPtChunkPages ? c.used - k * kPtChunkPages : kPtChunkPages;
        uint8_t *p = static_cast<uint8_t *>(c.chunk[k]->getBytesNoCopy());
        bzero(p, static_cast<size_t>(n) * 4096);
        for (uint32_t o = 0; o < n * 4096u; o += 64)
            flushLine(p + o);
    }
    if (c.hw) {
        vmFlushTlb(c.hw);
        hwv[c.hw - kFirstUserVmid].ctx = 0;
        c.hw = 0;
    }
    c.used = 1;
    c.busy = false;
    IOLockUnlock(gpuLock);
}

IOReturn PhxBringup::adoptQueue(const uint64_t in[6]) {
    if (!fbAper || !doorbellMap || !mmio || !fbMcBase || !gpuLock)
        return kIOReturnNotReady;
    // in[5] = fence offset | queue index (0 compute, 1 gfx): the fence is 8-byte aligned, so bit 0 is free
    const uint64_t ringOff = in[0], ringDw = in[1], wbOff = in[2], dbByte = in[3], wptr = in[4], fenceOff = in[5] & ~1ull;
    const uint32_t qi = static_cast<uint32_t>(in[5] & 1);
    if (ringDw < 256 || ringDw > 65536 || (ringDw & (ringDw - 1)) || (ringOff & 0xFF) ||
        ringOff + ringDw * 4 > fbAperLen || (wbOff & 7) || wbOff + 32 > fbAperLen || (fenceOff & 7) ||
        fenceOff + 8 > fbAperLen || (dbByte & 7) || dbByte + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    GpuQueue &q = gq[qi];
    bzero(&q, sizeof(q));
    q.gfx = qi == 1;
    q.ringOff = static_cast<uint32_t>(ringOff);
    q.ringDw = static_cast<uint32_t>(ringDw);
    q.wbOff = static_cast<uint32_t>(wbOff);
    q.fenceOff = static_cast<uint32_t>(fenceOff);
    q.dbByte = dbByte;
    q.wptr = q.doneEnd = wptr;
    volatile uint32_t *fence = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
    fence[0] = static_cast<uint32_t>(gSeq);
    fence[1] = static_cast<uint32_t>(gSeq >> 32);
    hdpFlush();
    q.ready = true;
    IOLockUnlock(gpuLock);
    IOLog("PhxBringup: adopted %s queue ring=+0x%llx (%llu dw) wptr=%llu doorbell=+0x%llx\n",
          qi ? "gfx" : "compute", ringOff, ringDw, wptr, dbByte);
    return kIOReturnSuccess;
}

// PhxAccel (the NullMoth accelerator in its AMD configuration) starts only on a GPU marked as driven; matching is
// re-run so the accelerator personality, refused at boot, is offered the device again.
IOReturn PhxBringup::claimAccel() {
    if (!pci || !gpuReady())
        return kIOReturnNotReady;
    pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci->registerService();
    IOLog("PhxBringup: GPU claimed for PhxAccel, matching re-run\n");
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vmMap(uint32_t handle, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        IOByteCount segLen = 0;
        const addr64_t pa = md->getPhysicalSegment(off + p, &segLen, kIOMemoryMapperNone);
        uint64_t *pte = pa ? vmPte(c, va + p, true) : nullptr;
        if (!pte) {
            r = pa ? kIOReturnNoMemory : kIOReturnVMError;
            break;
        }
        *pte = (pa & kPtAddrMask) | kPteSysFlags;
        flushLine(pte);
    }
    if (c.hw)   // an unbound context is flushed when it is next bound
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

IOReturn PhxBringup::vmUnmap(uint32_t handle, uint64_t va, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pte = vmPte(c, va + p, false);
        if (pte) {
            *pte = 0;
            flushLine(pte);
        }
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return kIOReturnSuccess;
}

void PhxBringup::ringPut(GpuQueue &q, uint32_t idx, uint32_t value) {
    const uint64_t slot = (q.wptr + idx) & (q.ringDw - 1);
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff + slot * 4) = value;
}

uint64_t PhxBringup::queueFence(const GpuQueue &q) const {
    const volatile uint32_t *f = reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + q.fenceOff);
    return static_cast<uint64_t>(f[0]) | (static_cast<uint64_t>(f[1]) << 32);
}

// Everything up to the returned sequence has finished: the oldest submission still running on any queue, minus one.
// Lock-free reader: an entry is written before `tail` moves past it.
uint64_t PhxBringup::gpuRetired() const {
    uint64_t r = gSeq;
    for (const GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++) {
            if (q.fseq[i & 63] > done) {
                if (q.fseq[i & 63] - 1 < r)
                    r = q.fseq[i & 63] - 1;
                break;
            }
        }
    }
    return r;
}

// Has this one submission finished? A queue's sequences only grow and its fence holds the newest one it completed, so
// a submission is done once its own queue has passed it, whatever the other queue does. gpuRetired() (everything up to
// r is done) stays the answer for releasing VMIDs and page tables; with it alone a hung gfx job made every later compute
// fence look unsignalled too (try6/try7: vkprobe on the compute queue timed out behind kexttest's stuck gfx IB).
// Only sequences already handed back by gpuSubmit are asked for: those are in a queue's history or retired from it.
bool PhxBringup::gpuSeqDone(uint64_t seq) const {
    if (seq <= gpuRetired())
        return true;
    for (const GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++)
            if (q.fseq[i & 63] == seq)
                return queueFence(q) >= seq;
    }
    return seq <= gSeq;
}

IOReturn PhxBringup::gpuSubmit(uint32_t handle, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    // graphics IBs go to the gfx queue and compute IBs to the compute queue; with one of them missing, the other runs both
    GpuQueue *qp = gfx ? (gq[1].ready ? &gq[1] : &gq[0]) : (gq[0].ready ? &gq[0] : &gq[1]);
    if (!qp->ready || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = *qp;
    const uint32_t need = n * 4 + 8;
    if (need + 16 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    // Ring space and the 64-entry history: retire finished entries, wait for the oldest one if needed (bounded, ~2 s).
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 16)
            break;
        if (spin > 40000) {
            IOLockUnlock(gpuLock);
            return kIOReturnBusy;
        }
        IODelay(50);
    }
    const uint32_t vmid = vmBind(handle);
    if (!vmid) {
        IOLockUnlock(gpuLock);
        return kIOReturnBusy;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ringPut(q, k++, 0xC0023F00);                               // INDIRECT_BUFFER
        // the CP wants the canonical (sign-extended) address: an upper-half IB (bit 47, where RADV puts them) with a
        // bare 16-bit high word silently wedges the queue
        const uint64_t cva = (ibVa[i] & (1ull << 47)) ? (ibVa[i] | 0xFFFF000000000000ull) : ibVa[i];
        ringPut(q, k++, static_cast<uint32_t>(cva) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(cva >> 32));
        // size | VMID, plus VALID on compute (gfx_v11_0_ring_emit_ib_compute; the gfx variant has no VALID bit)
        ringPut(q, k++, ibDw[i] | (q.gfx ? 0u : (1u << 23)) | (vmid << 24));
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0xC0064900);                                   // RELEASE_MEM, as gfx_v11_0_ring_emit_fence
    ringPut(q, k++, 0x06603514);                                   // GCR SEQ|GL2_WB|GLM_INV|GLM_WB, CP 3, CACHE_FLUSH_AND_INV_TS, idx 5
    ringPut(q, k++, 0x40000000);                                   // DATA_SEL 2 (64-bit), no interrupt
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, static_cast<uint32_t>(seq >> 32));
    ringPut(q, k++, 0);
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    // BAR0 is write-combined: drain the WC buffers so the packets are in VRAM before the CP is told about them
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    volatile uint32_t *wb = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.wbOff + 16);
    wb[0] = static_cast<uint32_t>(q.wptr);
    wb[1] = static_cast<uint32_t>(q.wptr >> 32);
    __asm__ __volatile__("sfence" ::: "memory");
    (void)wb[0];
    hdpFlush();
    __asm__ __volatile__("mfence" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + q.dbByte) = q.wptr;
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

// ---- scan-out copies by SDMA, double-buffered (build 36) -------------------------------------------------------
// Builds 34/35 copied the frame with CP DMA on the compute queue WindowServer's Metal runs on: a stuck copy stuck
// WindowServer too. SDMA is an engine of its own (Linux's TTM moves buffers with it), so a stuck copy here costs only the
// copies: the engine is halted and reset, the firmware's frame goes back on the panel, PhxAccel copies by CPU again.
// Strix Point only (the DCN 3.5 pipe 0 registers below, checked with mesboot sdmatest S5 on the 880M).
namespace {
// SDMA0 registers: GC base 0 / 1 + the GC 11.0 offsets, also right on 11.5 (Linux sdma_v6_0.c uses gc_11_0_0 for both)
constexpr uint32_t rSdSemWaitFail = RDNA3_GC_SEG0 + 0x22, rSdFreeze = RDNA3_GC_SEG0 + 0x2b;
constexpr uint32_t rSdUtcl1Cntl = RDNA3_GC_SEG0 + 0x3c, rSdUtcl1Page = RDNA3_GC_SEG0 + 0x3f;
constexpr uint32_t rSdRbCntl = RDNA3_GC_SEG0 + 0x80, rSdRbBase = RDNA3_GC_SEG0 + 0x81, rSdRbBaseHi = RDNA3_GC_SEG0 + 0x82;
constexpr uint32_t rSdRbRptr = RDNA3_GC_SEG0 + 0x83, rSdRbRptrHi = RDNA3_GC_SEG0 + 0x84;
constexpr uint32_t rSdRbWptr = RDNA3_GC_SEG0 + 0x85, rSdRbWptrHi = RDNA3_GC_SEG0 + 0x86;
constexpr uint32_t rSdRptrAddrHi = RDNA3_GC_SEG0 + 0x88, rSdRptrAddrLo = RDNA3_GC_SEG0 + 0x89, rSdIbCntl = RDNA3_GC_SEG0 + 0x8a;
constexpr uint32_t rSdDoorbell = RDNA3_GC_SEG0 + 0x92, rSdPreempt = RDNA3_GC_SEG0 + 0xb0;
constexpr uint32_t rSdPollHi = RDNA3_GC_SEG0 + 0xb2, rSdPollLo = RDNA3_GC_SEG0 + 0xb3, rSdMinorPtr = RDNA3_GC_SEG0 + 0xb5;
constexpr uint32_t rSdF32Cntl = RDNA3_GC_SEG1 + 0x589a, rGrbmSoftReset = RDNA3_GC_SEG0 + 0xda8;
// DCN 3.5 pipe 0 (DCN base 2)
constexpr uint32_t rHubpCntl = RDNA3_DCN_SEG2 + 0x5f3, rHubpViewport = RDNA3_DCN_SEG2 + 0x5ea, rHubpPitch = RDNA3_DCN_SEG2 + 0x607;
constexpr uint32_t rSurfAddr = RDNA3_DCN_SEG2 + 0x60a, rSurfAddrHi = RDNA3_DCN_SEG2 + 0x60b, rFlipCntl = RDNA3_DCN_SEG2 + 0x61b;
// the carve-out the copies use (below 200 MiB so the BAR reaches it; nothing else lives there: fw_buf ends at 80 MiB,
// the TMR sits 512 MiB below the top). mesboot sdmatest refuses to run while queue 0 is enabled.
constexpr uint64_t kSdRingOff = 0x6000000ull, kSdWbOff = 0x6010000ull, kSdIbOff = 0x6020000ull, kSdTestOff = 0x6100000ull;
constexpr uint32_t kSdRingDw = 16384;
constexpr uint64_t kScanBufOff[2] = {0x8000000ull, 0xA000000ull}, kScanBufMax = 32ull << 20;
constexpr uint32_t kBlitVmid = 7;
constexpr uint64_t kSdVaIb = 0x10000000ull, kSdVaTest = 0x20000000ull, kSdVaTestSrc = 0x30000000ull;
constexpr uint64_t kSdVaBuf[2] = {0x40000000ull, 0x44000000ull};
}

static int phxscanout_selftest_sysctl SYSCTL_HANDLER_ARGS {
    int v = gScanoutSelftestRes;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (v == 1 && gBringup)
        gScanoutSelftestRes = gBringup->scanoutSelftest();
    return 0;
}

static void scanoutState(const char *s) {
    snprintf(gScanoutState, sizeof(gScanoutState), "%s", s);
}

// The VMID 7 context's local (carve-out) pages: the GART's PTE flags (VALID EXEC READ WRITE, MTYPE 0), physical address.
IOReturn PhxBringup::vmMapLocal(uint32_t handle, uint64_t va, uint64_t phys, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pte = vmPte(c, va + p, true);
        if (!pte) {
            r = kIOReturnNoMemory;
            break;
        }
        *pte = ((phys + p) & kPtAddrMask) | 0x71ull;
        flushLine(pte);
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

void PhxBringup::sdPut(uint32_t v) {
    vram32(kSdRingOff + static_cast<uint64_t>(sdWp & (kSdRingDw - 1)) * 4) = v;
    sdWp++;
}

// the write pointer, in bytes, by register (no doorbell), and its shadow for F32 polling (sdma_v6_0_ring_set_wptr)
void PhxBringup::sdKick() {
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    vram32(kSdWbOff + 0x10) = sdWp * 4;
    vram32(kSdWbOff + 0x14) = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wreg(rSdRbWptr, sdWp * 4);
    wreg(rSdRbWptrHi, 0);
}

void PhxBringup::sdmaStop(bool reset) {
    wreg(rSdRbCntl, rreg(rSdRbCntl) & ~1u);
    wreg(rSdIbCntl, rreg(rSdIbCntl) & ~1u);
    if (reset) {   // sdma_v6_0_soft_reset, SDMA0 only
        wreg(rSdFreeze, rreg(rSdFreeze) | 0x10u);
        wreg(rSdF32Cntl, rreg(rSdF32Cntl) | 1u | (1u << 13));   // HALT, TH1_RESET
        wreg(rSdPreempt, 0);
        IODelay(100);
        wreg(rGrbmSoftReset, 1u << 23);
        (void)rreg(rGrbmSoftReset);
        IODelay(100);
        wreg(rGrbmSoftReset, 0);
        (void)rreg(rGrbmSoftReset);
        IODelay(100);
        wreg(rSdFreeze, rreg(rSdFreeze) & ~0x10u);
    }
    sdmaUp = false;
}

// Queue 0 as sdma_v6_0_gfx_resume_instance programs it (ring 64 KiB, rptr write-back, wptr by register), then one
// FENCE to see it run.
bool PhxBringup::sdmaStart() {
    if (sdmaUp)
        return true;
    if (rreg(rSdRbCntl) & 1u) {
        IOLog("PhxBringup: SDMA0 queue 0 is enabled by someone else (mesboot sdmatest?)\n");
        return false;
    }
    for (uint32_t i = 0; i < 0x40; i += 4)
        vram32(kSdWbOff + i) = 0;
    for (uint32_t i = 0; i < 256; i += 4)
        vram32(kSdRingOff + i) = 0;
    const uint64_t ring = fbMcBase + kSdRingOff, wb = fbMcBase + kSdWbOff;
    wreg(rSdSemWaitFail, 0);
    uint32_t rb = rreg(rSdRbCntl) & ~1u;
    rb = (rb & ~(0x1fu << 1)) | (14u << 1);   // RB_SIZE log2(16384 dwords)
    rb = (rb | (1u << 23)) & ~(0xfu << 24);   // RB_PRIV, RB_VMID 0
    wreg(rSdRbCntl, rb);
    wreg(rSdRbRptr, 0);
    wreg(rSdRbRptrHi, 0);
    wreg(rSdRbWptr, 0);
    wreg(rSdRbWptrHi, 0);
    wreg(rSdPollLo, static_cast<uint32_t>(wb + 0x10));
    wreg(rSdPollHi, static_cast<uint32_t>((wb + 0x10) >> 32));
    wreg(rSdRptrAddrHi, static_cast<uint32_t>(wb >> 32));
    wreg(rSdRptrAddrLo, static_cast<uint32_t>(wb) & ~3u);
    rb = (rb | (1u << 12) | (1u << 11)) & ~(1u << 8);   // RPTR_WRITEBACK, F32_WPTR_POLL; no WPTR_POLL
    wreg(rSdRbBase, static_cast<uint32_t>(ring >> 8));
    wreg(rSdRbBaseHi, static_cast<uint32_t>(ring >> 40));
    wreg(rSdMinorPtr, 1);
    wreg(rSdRbWptr, 0);
    wreg(rSdRbWptrHi, 0);
    wreg(rSdDoorbell, rreg(rSdDoorbell) & ~(1u << 28));
    wreg(rSdMinorPtr, 0);
    wreg(rSdUtcl1Cntl, (rreg(rSdUtcl1Cntl) & ~0x61fu) | (3u << 9) | 9u);                  // RESP_MODE 3, REDO_DELAY 9
    wreg(rSdUtcl1Page, (rreg(rSdUtcl1Page) & 0xFF0FFFu) | (2u << 12) | (3u << 14) | (1u << 24));   // RD NOA, WR BYPASS
    wreg(rSdF32Cntl, rreg(rSdF32Cntl) & ~(1u | (1u << 13)));                                 // unhalt
    wreg(rSdRbCntl, rb | 1u);
    wreg(rSdIbCntl, rreg(rSdIbCntl) | 1u);
    sdWp = 0;
    sdSeq = 0;
    sdmaUp = true;
    const uint64_t f = fbMcBase + kSdWbOff + 0x20;
    sdPut(5u | (3u << 16));   // FENCE, MTYPE UC
    sdPut(static_cast<uint32_t>(f));
    sdPut(static_cast<uint32_t>(f >> 32));
    sdPut(++sdSeq);
    sdKick();
    for (int i = 0; i < 1000; i++) {
        if (vram32(kSdWbOff + 0x20) == sdSeq) {
            IOLog("PhxBringup: SDMA0 queue 0 up\n");
            return true;
        }
        IODelay(50);
    }
    IOLog("PhxBringup: SDMA0 queue 0 did not run its first FENCE\n");
    sdmaStop(true);
    return false;
}

// One IB in VMID 7 (ibDw dwords at kSdIbOff), a GL2 write-back and a FENCE behind it; waits up to timeoutMs. A timeout
// halts and resets the engine.
IOReturn PhxBringup::sdRun(uint32_t ibDw, uint32_t timeoutMs) {
    while ((sdWp + 6) & 7)
        sdPut(0);   // NOP: the IB packet ends on an 8-dword boundary
    sdPut(4u | (kBlitVmid << 16));   // INDIRECT
    sdPut(static_cast<uint32_t>(kSdVaIb));
    sdPut(static_cast<uint32_t>(kSdVaIb >> 32));
    sdPut(ibDw);
    sdPut(0);
    sdPut(0);
    const uint32_t g = (1u << 15) | (1u << 14) | (1u << 9) | (1u << 8) | (1u << 7) | (1u << 5) | 1u;   // as emit_mem_sync
    sdPut(17u);   // GCR_REQ
    sdPut(0);
    sdPut((g & 0xffff) << 16);
    sdPut((g >> 16) & 7);
    sdPut(0);
    const uint64_t f = fbMcBase + kSdWbOff + 0x20;
    const uint32_t seq = ++sdSeq;
    sdPut(5u | (3u << 16));
    sdPut(static_cast<uint32_t>(f));
    sdPut(static_cast<uint32_t>(f >> 32));
    sdPut(seq);
    sdKick();
    uint64_t limit = 0, t0 = mach_absolute_time();
    nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutMs) * 1000 * 1000, &limit);
    while (static_cast<int32_t>(vram32(kSdWbOff + 0x20) - seq) < 0) {
        if (mach_absolute_time() - t0 > limit) {
            IOLog("PhxBringup: SDMA copy %u did not finish in %u ms (rptr %u wptr %u): engine halted and reset\n", seq,
                  timeoutMs, vram32(kSdWbOff), sdWp * 4);
            sdmaStop(true);
            return kIOReturnTimeout;
        }
        IODelay(5);
    }
    return kIOReturnSuccess;
}

void PhxBringup::scanToGop() {
    if (scanFront < 0)
        return;
    wreg(rSurfAddrHi, gopHi);
    wreg(rSurfAddr, gopLo);   // the low word last: it latches the flip
    scanFront = -1;
}

IOReturn PhxBringup::blitFail(const char *why, IOReturn r) {
    scanToGop();
    blitBroken = why;
    char m[96];
    snprintf(m, sizeof(m), "off until reboot: %s", why);
    scanoutState(m);
    IOLog("PhxBringup: scan-out copies %s (0x%x); the firmware's frame is back on the panel\n", m, r);
    return r;
}

void PhxBringup::blitReleaseAll() {
    if (mmio)
        scanToGop();
    if (sdmaUp && mmio)
        sdmaStop(false);
    for (BlitSrc &b : blitSrc) {
        if (b.md) {
            if (blitCtx)
                vmUnmap(blitCtx, kBlitSrcVa + static_cast<uint64_t>(&b - blitSrc) * kBlitSlotSpan, b.len);
            b.md->complete();
            b.md->release();
        }
        b = BlitSrc{};
    }
    if (blitCtx && mmio) {   // VMID 7 back to an empty context, as the bring-up left it
        const rdna3_gcvm_regs &g = chip->gcvm;
        wreg(g.ctx0_pt_base_lo + 2 * kBlitVmid, 0);
        wreg(g.ctx0_pt_base_lo + 2 * kBlitVmid + 1, 0);
        vmFlushTlb(kBlitVmid);
    }
    blitCtx = 0;   // its page tables go with the rest in stop()
}

// The VMID 7 context (page tables in system memory, from the client pool), the IB page and the two scan-out buffers
// mapped as local memory, and SDMA up (blitLock held).
bool PhxBringup::blitSetup() {
    if (blitBroken || !fbAper || fbAperLen < kScanBufOff[1] + kScanBufMax || !fbMcBase || !fbPhys)
        return false;
    if (!blitCtx) {
        if (!(blitCtx = vmidAlloc()))
            return false;
        VmCtx &c = vm[blitCtx - 1];
        const rdna3_gcvm_regs &g = chip->gcvm;
        IOLockLock(gpuLock);
        wreg(g.ctx0_pt_base_lo + 2 * kBlitVmid, static_cast<uint32_t>(c.phys | 3));
        wreg(g.ctx0_pt_base_lo + 2 * kBlitVmid + 1, static_cast<uint32_t>(c.phys >> 32));
        wreg(g.ctx0_cntl + kBlitVmid, 0x01fffe07);
        wreg(g.ctx0_pt_start_lo + 2 * kBlitVmid, 0);
        wreg(g.ctx0_pt_start_lo + 2 * kBlitVmid + 1, 0);
        wreg(g.ctx0_pt_end_lo + 2 * kBlitVmid, 0xffffffff);
        wreg(g.ctx0_pt_end_lo + 2 * kBlitVmid + 1, 0xf);
        c.hw = kBlitVmid;   // never bound through vmBind: 7 is not one of the lent VMIDs
        vmFlushTlb(kBlitVmid);
        IOLockUnlock(gpuLock);
        if (vmMapLocal(blitCtx, kSdVaIb, fbPhys + kSdIbOff, 4096) != kIOReturnSuccess ||
            vmMapLocal(blitCtx, kSdVaTest, fbPhys + kSdTestOff, 1u << 20) != kIOReturnSuccess ||
            vmMapLocal(blitCtx, kSdVaBuf[0], fbPhys + kScanBufOff[0], kScanBufMax) != kIOReturnSuccess ||
            vmMapLocal(blitCtx, kSdVaBuf[1], fbPhys + kScanBufOff[1], kScanBufMax) != kIOReturnSuccess) {
            blitFail("could not map the scan-out buffers", kIOReturnNoMemory);
            return false;
        }
    }
    if (!sdmaStart()) {
        blitFail("SDMA0 did not start", kIOReturnNotReady);
        return false;
    }
    return true;
}

// Pipe 0 must scan the firmware's frame with the geometry of the frames to come (blitLock held). Remembers where the
// firmware's frame is, to put it back.
bool PhxBringup::scanSetup(uint64_t bytes, uint64_t gopOffIn) {
    if (gopOff == gopOffIn && scanBytes == bytes)
        return true;
    if (scanFront >= 0) {
        blitFail("the frame size changed under a flip", kIOReturnBadArgument);
        return false;
    }
    const uint32_t hi = rreg(rSurfAddrHi), lo = rreg(rSurfAddr), vp = rreg(rHubpViewport), pr = rreg(rHubpPitch);
    const uint64_t addr = (static_cast<uint64_t>(hi & 0xffff) << 32) | lo;
    const uint32_t h = (vp >> 16) & 0x3fff, pitchPx = (pr & 0x3fff) + 1;
    if (addr != fbMcBase + gopOffIn || static_cast<uint64_t>(pitchPx) * 4 * h != bytes || bytes > kScanBufMax) {
        IOLog("PhxBringup: pipe 0 scans 0x%llx (%u rows, pitch %u px), not the firmware's frame at 0x%llx of %llu bytes\n",
              addr, h, pitchPx, fbMcBase + gopOffIn, bytes);
        blitFail("pipe 0 is not on the firmware's frame", kIOReturnUnsupported);
        return false;
    }
    gopHi = hi;
    gopLo = lo;
    gopOff = gopOffIn;
    scanBytes = bytes;
    return true;
}

IOReturn PhxBringup::scanoutCopy(IOMemoryDescriptor *src, uint64_t bytes, uint64_t gopOffIn, const ScanArgs *chk, uint64_t *usOut) {
    *usOut = 0;
    if (chip->id != RDNA3_CHIP_STRIX_POINT)
        return kIOReturnUnsupported;
    if (!gpuReady() || !blitLock || !src || !bytes || (bytes & 3) || bytes > kScanBufMax || src->getLength() < bytes)
        return kIOReturnBadArgument;
    if (blitBroken)
        return kIOReturnNotReady;
    const uint64_t t0 = mach_absolute_time();
    IOLockLock(blitLock);
    IOReturn r = kIOReturnSuccess;
    if (!blitSetup() || !scanSetup(bytes, gopOffIn))
        r = kIOReturnNotReady;
    // an underflow since the last flip: the panel may be showing garbage, back to the firmware's frame
    if (r == kIOReturnSuccess && scanFront >= 0 && ((rreg(rHubpCntl) >> 28) & 7))
        r = blitFail("pipe 0 underflowed", kIOReturnIOError);
    // the buffer to fill must not be the one on the panel: the last flip has to have landed (SURFACE_FLIP_PENDING)
    const int back = scanFront == 0 ? 1 : 0;
    if (r == kIOReturnSuccess) {
        for (int i = 0; i < 500 && (rreg(rFlipCntl) & 0x100u); i++)
            IODelay(50);
        if (rreg(rFlipCntl) & 0x100u) {
            OSAddAtomic64(1, &gScanoutBusy);
            r = kIOReturnBusy;
        }
    }
    // The frame must be finished: WindowServer hands it over once its composite is submitted, not done (the plugin's
    // pending/ready marks never come on the 880M: nvaccel_surface_gpu_marks 0), and SDMA copied Launchpad's frames half
    // drawn (2026-10-26 23:11). Wait for everything submitted so far, 50 ms at most; past that, copy anyway.
    if (r == kIOReturnSuccess) {
        const uint64_t target = gSeq, w0 = mach_absolute_time();
        uint64_t lim = 0;
        nanoseconds_to_absolutetime(50ull * 1000 * 1000, &lim);
        while (gpuRetired() < target && mach_absolute_time() - w0 < lim)
            IODelay(20);
        if (gpuRetired() < target)
            OSAddAtomic64(1, &gScanoutGpuLate);
        uint64_t wns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - w0, &wns);
        OSAddAtomic64(static_cast<SInt64>(wns / 1000), &gScanoutGpuWaitUs);
    }
    // the frame: a slot it already has, else the least recently used one
    const uint64_t srcSpan = round_page_64(bytes);
    uint32_t slot = kBlitSlots;
    if (r == kIOReturnSuccess) {
        for (uint32_t i = 0; i < kBlitSlots; i++)
            if (blitSrc[i].md == src && blitSrc[i].len >= srcSpan)
                slot = i;
        if (slot == kBlitSlots) {
            slot = 0;
            for (uint32_t i = 0; i < kBlitSlots; i++) {
                if (!blitSrc[i].md) {
                    slot = i;
                    break;
                }
                if (blitSrc[i].lastUse < blitSrc[slot].lastUse)
                    slot = i;
            }
            BlitSrc &b = blitSrc[slot];
            const uint64_t va = kBlitSrcVa + slot * kBlitSlotSpan;
            if (b.md) {
                vmUnmap(blitCtx, va, b.len);
                b.md->complete();
                b.md->release();
                b = BlitSrc{};
            }
            const uint64_t mapLen = round_page_64(src->getLength()) < srcSpan ? round_page_64(src->getLength()) : srcSpan;
            if (src->prepare() != kIOReturnSuccess) {
                r = kIOReturnVMError;
            } else if (vmMap(blitCtx, va, src, 0, mapLen) != kIOReturnSuccess) {
                vmUnmap(blitCtx, va, mapLen);
                src->complete();
                r = kIOReturnNoMemory;
            } else {
                src->retain();
                b.md = src;
                b.len = mapLen;
            }
        }
        if (r == kIOReturnSuccess)
            blitSrc[slot].lastUse = ++blitTick;
    }
    if (r == kIOReturnSuccess) {   // COPY_LINEAR frame -> back buffer, one packet (up to 1 GiB)
        const uint64_t sv = kBlitSrcVa + slot * kBlitSlotSpan, dv = kSdVaBuf[back];
        const uint32_t ib[8] = {1u, static_cast<uint32_t>(bytes - 1), 0, static_cast<uint32_t>(sv), static_cast<uint32_t>(sv >> 32),
                                static_cast<uint32_t>(dv), static_cast<uint32_t>(dv >> 32), 0};
        for (uint32_t i = 0; i < 8; i++)
            vram32(kSdIbOff + i * 4) = ib[i];
        if (sdRun(8, 50) != kIOReturnSuccess)
            r = blitFail("an SDMA copy timed out", kIOReturnTimeout);
    }
    // 32 pixels of the copy against the frame, read now through the caller's mapping (lines flushed first), before
    // anyone sees it. A pixel that differs is read again: if the frame itself changed meanwhile, the GPU is still
    // drawing it - dropped, not counted against SDMA; if not, the copy is wrong - dropped, and eight in a row switch
    // the path off.
    if (r == kIOReturnSuccess && gScanoutVerify && chk && chk->srcKva) {
        const volatile uint8_t *sp = reinterpret_cast<const volatile uint8_t *>(chk->srcKva);
        for (uint32_t i = 0; i < chk->n && i < 32; i++) {
            const uint64_t off = chk->off[i] & ~3u;
            if (off + 4 > bytes)
                continue;
            flushLine(sp + off);
            __asm__ __volatile__("mfence" ::: "memory");
            const uint32_t want = *reinterpret_cast<const volatile uint32_t *>(sp + off);
            const uint32_t got = vram32(kScanBufOff[back] + off);
            if (got == want)
                continue;
            IODelay(50);
            flushLine(sp + off);
            __asm__ __volatile__("mfence" ::: "memory");
            const uint32_t again = *reinterpret_cast<const volatile uint32_t *>(sp + off);
            if (again != want) {
                OSAddAtomic64(1, &gScanoutMoving);
                r = kIOReturnBusy;
                break;
            }
            OSAddAtomic64(1, &gScanoutMismatch);
            snprintf(gScanoutLastBad, sizeof(gScanoutLastBad), "+0x%llx: copied 0x%08x, source 0x%08x (run %u)",
                     off, got, want, badRun + 1);
            if (++badRun >= 8)
                r = blitFail("8 copied frames in a row read back wrong", kIOReturnIOError);
            else
                r = kIOReturnBusy;   // dropped: no flip, the caller keeps the panel as it is
            break;
        }
        if (r == kIOReturnSuccess)
            badRun = 0;
    }
    if (r == kIOReturnSuccess) {   // flip: latched at the next vblank (DCSURF_FLIP_CONTROL.SURFACE_FLIP_TYPE 0)
        const uint64_t mc = ((static_cast<uint64_t>(gopHi & 0xffff) << 32) | gopLo) - gopOff + kScanBufOff[back];
        wreg(rSurfAddrHi, static_cast<uint32_t>(mc >> 32));
        wreg(rSurfAddr, static_cast<uint32_t>(mc));
        if (scanFront < 0)
            scanoutState("on");
        scanFront = back;
    }
    IOLockUnlock(blitLock);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    *usOut = ns / 1000;
    if (r == kIOReturnSuccess) {
        OSAddAtomic64(1, &gScanoutCopies);
        OSAddAtomic64(static_cast<SInt64>(ns / 1000), &gScanoutUsSum);
    }
    return r;
}

void PhxBringup::scanoutRelease() {
    if (!blitLock)
        return;
    IOLockLock(blitLock);
    scanToGop();
    IOLockUnlock(blitLock);
}

int PhxBringup::scanoutSelftest() {
    static constexpr uint64_t kBytes = 1u << 20;
    if (chip->id != RDNA3_CHIP_STRIX_POINT || !gpuReady() || !blitLock)
        return 4;
    if (blitBroken)
        return 3;
    IOBufferMemoryDescriptor *sb = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kBytes, PAGE_SIZE);
    if (!sb)
        return 4;
    uint32_t *sp = static_cast<uint32_t *>(sb->getBytesNoCopy());
    for (uint32_t i = 0; i < kBytes / 4; i++)
        sp[i] = 0x9E3779B1u * (i + 1);
    int res = 4;
    IOLockLock(blitLock);
    if (blitSetup() && vmMap(blitCtx, kSdVaTestSrc, sb, 0, kBytes) == kIOReturnSuccess) {
        for (uint64_t o = 0; o < kBytes; o += 4)
            vram32(kSdTestOff + o) = 0xDEADBEEFu;
        const uint32_t ib[8] = {1u, static_cast<uint32_t>(kBytes - 1), 0, static_cast<uint32_t>(kSdVaTestSrc),
                                static_cast<uint32_t>(kSdVaTestSrc >> 32), static_cast<uint32_t>(kSdVaTest),
                                static_cast<uint32_t>(kSdVaTest >> 32), 0};
        for (uint32_t i = 0; i < 8; i++)
            vram32(kSdIbOff + i * 4) = ib[i];
        const uint64_t t0 = mach_absolute_time();
        const IOReturn r = sdRun(8, 50);
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        if (r == kIOReturnTimeout) {
            res = 3;
            blitFail("the self-test copy timed out", r);
        } else if (r == kIOReturnSuccess) {
            uint32_t bad = 0;
            for (uint32_t i = 0; i < kBytes / 4; i += 7)
                bad += vram32(kSdTestOff + i * 4) != 0x9E3779B1u * (i + 1);
            bad += vram32(kSdTestOff + kBytes - 4) != 0x9E3779B1u * static_cast<uint32_t>(kBytes / 4);
            res = bad ? 2 : 1;
            IOLog("PhxBringup: SDMA self-test: 1 MiB system RAM -> carve-out in %llu us, %u samples wrong\n", ns / 1000, bad);
        }
        vmUnmap(blitCtx, kSdVaTestSrc, kBytes);
    }
    IOLockUnlock(blitLock);
    sb->release();   // the copy finished or the engine was reset: nothing reads the pages any more
    return res;
}

static int phxdmcub_sysctl SYSCTL_HANDLER_ARGS {
    return gBringup ? gBringup->dmcubDump(req) : ENODEV;
}

int PhxBringup::dmcubDump(struct sysctl_req *req) {
    static constexpr uint32_t kReg0 = 0x3580, kRegs = 0x180;
    static constexpr uint64_t kWin = 1u << 20;
    const uint64_t total = 16 + kRegs * 4 + kWin;
    if (!req->oldptr)
        return SYSCTL_OUT(req, nullptr, total);   // size query
    if (!mmio || chip->id != RDNA3_CHIP_STRIX_POINT || !fbMcBase || !fbPhys)
        return ENODEV;
    uint8_t *buf = static_cast<uint8_t *>(IOMalloc(total));
    if (!buf)
        return ENOMEM;
    bzero(buf, total);
    uint32_t *h = reinterpret_cast<uint32_t *>(buf);
    h[0] = 0x42434D44;   // 'DMCB'
    h[1] = kReg0;
    const uint32_t *regs = h + 4;
    for (uint32_t i = 0; i < kRegs; i++)
        h[4 + i] = rreg(kReg0 + i);
    // region 4 (DMCUB_REGION4_OFFSET / _HIGH at 0x3656 / 0x3657): an MC address in the carve-out
    const uint64_t r4 = (static_cast<uint64_t>(regs[0x3657 - kReg0]) << 32) | regs[0x3656 - kReg0];
    uint64_t off = r4 >= fbMcBase ? r4 - fbMcBase : ~0ull;
    if (off == ~0ull || off + kWin > fbSize)
        off = fbSize > kWin ? fbSize - kWin : 0;   // fall back to the top MiB, where the GOP put it on the 880M
    h[2] = static_cast<uint32_t>(off);
    h[3] = static_cast<uint32_t>(off >> 32);
    int err = 0;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(fbPhys + off), kWin, kIODirectionIn);
    IOMemoryMap *map = md ? md->map(kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    if (map) {
        const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
        uint32_t *dst = reinterpret_cast<uint32_t *>(buf + 16 + kRegs * 4);
        for (uint64_t i = 0; i < kWin / 4; i++)
            dst[i] = src[i];
    } else {
        err = ENOMEM;
    }
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(md);
    if (!err)
        err = SYSCTL_OUT(req, buf, total);
    IOFree(buf, total);
    return err;
}

IOReturn PhxBringup::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                          void *param3, void *param4) {
    if (functionName && functionName->isEqualTo("phxScanoutCopy")) {
        uint64_t us = 0;
        const uint32_t magic = param4 ? *static_cast<uint32_t *>(param4) : 0;
        ScanArgs *a = magic == 0x32415350u ? static_cast<ScanArgs *>(param4) : nullptr;   // 'PSA2'
        const IOReturn r = scanoutCopy(static_cast<IOMemoryDescriptor *>(param1), reinterpret_cast<uintptr_t>(param2),
                                       reinterpret_cast<uintptr_t>(param3), a, &us);
        if (a)
            a->us = us;
        else if (magic == 0x31415350u)   // 'PSA1' (PhxAccel 39): us sits at +8 there too
            static_cast<ScanArgs *>(param4)->us = us;
        else if (param4)
            *static_cast<uint64_t *>(param4) = us;
        return r;
    }
    if (functionName && functionName->isEqualTo("phxScanoutRelease")) {
        scanoutRelease();
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

IOReturn PhxBringup::doorbellWrite(uint64_t byteOff, uint64_t value) {
    if (!doorbellMap)
        return kIOReturnNotReady;
    if ((byteOff & 7) || byteOff + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + byteOff) = value;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vramWrite(uint32_t byteOff, uint32_t value) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || static_cast<uint64_t>(byteOff) + 4 > fbAperLen)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff) = value;
    __asm__ __volatile__("sfence" ::: "memory");
    return kIOReturnSuccess;
}

// psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK): response flag set and status 0.
bool PhxBringup::pspWaitTos(uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs * 10; i++) {
        if ((rreg(PHX_PSP_C2PMSG_64) & PHX_MBOX_TOS_MASK) == PHX_MBOX_TOS_READY)
            return true;
        IODelay(100);
    }
    return false;
}

// Debug selector 101: create / destroy the PSP 13.0.4 KM ring the way Linux psp_v13_0_ring_create / _stop and
// Cobalt64 psp_v14_0.cpp do. The ring lives in VRAM at carveOffset; the CPU writes it through the BAR0
// framebuffer aperture, which is MMIO above system RAM — so the PSP sees the bytes and the macOS heap is never
// touched. The MC address handed to the PSP is fbMcBase + carveOffset.
IOReturn PhxBringup::pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]) {
    if (!mmio || !fbAper || !fbMcBase)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    // Phoenix's TMR sits at 20 MiB; Strix's is 512 MiB below the top of VRAM, so there the ring may go anywhere below
    // fw_buf: bringup5.sh puts it at 48 MiB, out of a 3072x1920 firmware frame at offset 0 (build 37)
    const uint64_t ringLimit = chip->id == RDNA3_CHIP_STRIX_POINT ? PHX_VRAM_FWBUF_OFF : PHX_VRAM_TMR_OFF;
    IOLockLock(smuLock);
    if (op == 1) {
        if (ringMc) {
            r = kIOReturnExclusiveAccess;   // a ring is already live; destroy it first
        } else if ((carveOffset & 0xFFF) || carveOffset + PHX_PSP_FENCE_REL + 0x1000 > ringLimit ||
                   PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE > fbAperLen) {
            r = kIOReturnBadArgument;        // ring+cmd+fence must sit below the TMR (fw_buf on Strix); fw_buf must fit the BAR
        } else {
            volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + carveOffset);
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                nonzero |= ring[i];
            out[2] = nonzero == 0;           // report whether the VRAM region was already clear (informational)
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                ring[i] = 0;                 // zero the ring buffer (Linux memsets ring_mem; safe via BAR0)
            if (!pspWaitTos(2000)) {
                r = kIOReturnNotReady;       // SOS not ready to accept the ring
            } else {
                ringMc = fbMcBase + carveOffset;
                wreg(PHX_PSP_C2PMSG_69, static_cast<uint32_t>(ringMc));
                wreg(PHX_PSP_C2PMSG_70, static_cast<uint32_t>(ringMc >> 32));
                wreg(PHX_PSP_C2PMSG_71, PHX_PSP_RING_SIZE);
                wreg(PHX_PSP_C2PMSG_64, PHX_PSP_RING_TYPE_KM << 16);
                IOSleep(20);
                if (!pspWaitTos(2000)) {
                    r = kIOReturnTimeout;    // raw C2PMSG_64 (with PSP status in bits 15:0) returned in out[0]
                    ringMc = 0;              // not established: allow destroy + retry
                } else {
                    // Ring is live. Lay the GPCOM command + fence buffers just above it and zero them,
                    // then reset the doorbell so wptr agrees with our slot 0 (Linux psp_v13_0_ring_create).
                    ringOff = carveOffset;
                    cmdOff = carveOffset + PHX_PSP_CMD_REL;
                    fenceOff = carveOffset + PHX_PSP_FENCE_REL;
                    cmdMc = fbMcBase + cmdOff;
                    fenceMc = fbMcBase + fenceOff;
                    fenceCounter = 0;
                    tmrOff = PHX_VRAM_TMR_OFF;
                    tmrMc = fbMcBase + tmrOff;
                    tmrSize = 0;
                    tmrSetUp = false;
                    fwBufOff = PHX_VRAM_FWBUF_OFF;
                    fwBufMc = fbMcBase + fwBufOff;
                    fwBufSize = PHX_VRAM_FWBUF_SIZE;
                    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
                    for (uint32_t i = 0; i < 0x1000 / 4; i++) {
                        cmd[i] = 0;
                        fen[i] = 0;
                    }
                    wreg(PHX_PSP_C2PMSG_67, 0);
                }
            }
        }
    } else if (op == 2) {
        wreg(PHX_PSP_C2PMSG_64, PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS);
        IOSleep(20);
        if (!pspWaitTos(2000))
            r = kIOReturnTimeout;
        clearRing();
    } else if (op != 3) {
        r = kIOReturnBadArgument;
    }
    out[0] = rreg(PHX_PSP_C2PMSG_64);
    out[1] = ringMc;
    out[3] = rreg(PHX_PSP_C2PMSG_67);
    IOLockUnlock(smuLock);
    return r;
}

// Core GPCOM submit (caller holds smuLock and a ring exists): write a prepared command — cmd_id plus up to
// nWords union dwords at +28 — into the VRAM command buffer, ring the doorbell, wait for the PSP's fence and
// read the response. Ports Linux psp_cmd_submit_buf / Cobalt64 psp_ring_cmd_submit.
IOReturn PhxBringup::pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]) {
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(base + cmdOff);
    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(base + fenceOff);

    // Command buffer: zero it, set cmd_id (buf_size/buf_version stay 0, as upstream does), then the union.
    for (uint32_t i = 0; i < PHX_PSP_CMD_BUF_SIZE / 4; i++)
        cmd[i] = 0;
    cmd[PHX_PSP_CMD_HDR_CMD_ID_OFFSET / 4] = cmdId;
    for (uint32_t i = 0; i < nWords && i < 14; i++)
        cmd[PHX_PSP_GFX_CMD_UNION_OFFSET / 4 + i] = unionWords[i];

    uint32_t fence = ++fenceCounter;
    fen[0] = 0;

    // Ring frame at the slot the current wptr points to.
    uint32_t wptr = rreg(PHX_PSP_C2PMSG_67);
    const uint32_t ringSizeDw = PHX_PSP_RING_SIZE / 4;         // 1024
    const uint32_t frameSizeDw = PHX_PSP_GFX_FRAME_SIZE / 4;   // 16
    uint32_t slot = (wptr % ringSizeDw == 0) ? 0 : wptr / frameSizeDw;
    if (slot >= PHX_PSP_RING_SIZE / PHX_PSP_GFX_FRAME_SIZE)
        return kIOReturnInternalError;
    PhxGfxRbFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(cmdMc);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(cmdMc >> 32);
    frame.fence_addr_lo = static_cast<uint32_t>(fenceMc);
    frame.fence_addr_hi = static_cast<uint32_t>(fenceMc >> 32);
    frame.fence_value = fence;
    volatile uint32_t *slotp =
        reinterpret_cast<volatile uint32_t *>(base + ringOff + static_cast<uint64_t>(slot) * PHX_PSP_GFX_FRAME_SIZE);
    const uint32_t *src = reinterpret_cast<const uint32_t *>(&frame);
    for (uint32_t i = 0; i < PHX_PSP_GFX_FRAME_SIZE / 4; i++)
        slotp[i] = src[i];

    // Drain CPU write buffers, flush the host data path, then ring the doorbell.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wptr = (wptr + frameSizeDw) % ringSizeDw;
    wreg(PHX_PSP_C2PMSG_67, wptr);

    // Wait for the PSP to write the fence value into VRAM (BAR0 read bypasses the CPU cache).
    uint32_t observed = 0, us = 0;
    const uint32_t budgetUs = 5000000;
    while (us < budgetUs) {
        observed = fen[0];
        if (observed == fence)
            break;
        IOSleep(1);
        us += 1000;
    }
    uint32_t status = cmd[PHX_PSP_GFX_RESP_OFFSET / 4];
    // Always succeed at the transport level so IOKit delivers the outputs; the PSP result is in out[].
    // out[1] = elapsed us (≈budget on a fence timeout), so userspace distinguishes timeout from a status code.
    out[0] = (observed == fence) ? status : 0xFFFFFFFFu;  // 0xFFFFFFFF = fence never arrived
    out[1] = us;
    out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 8) / 4];   // resp.fw_addr_lo / session_id
    out[3] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 12) / 4];  // resp.fw_addr_hi
    return kIOReturnSuccess;
}

// Selector 103: no-payload query command (FB_FW_RESERV_ADDR/EXT only) — validates the ring without touching GPU power.
IOReturn PhxBringup::pspSubmit(uint32_t cmdId, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR && cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR)
        return kIOReturnNotPermitted;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, nullptr, 0, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Selector 104: stage a chunk of firmware bytes into the VRAM fw_buf through BAR0 (no submit).
IOReturn PhxBringup::fwStage(uint32_t dstOffset, const void *bytes, uint32_t len) {
    if (!fbAper || !fwBufSize)
        return kIOReturnNotReady;
    if ((dstOffset & 3) || static_cast<uint64_t>(dstOffset) + len > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress() + fwBufOff + dstOffset);
    const uint8_t *s = static_cast<const uint8_t *>(bytes);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    if (i < len) {
        uint32_t v = 0;
        memcpy(&v, s + i, len - i);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    IOLockUnlock(smuLock);
    return kIOReturnSuccess;
}

// Selector 105: submit GFX_CMD_ID_LOAD_IP_FW for the firmware already staged in fw_buf at fwBufOffset.
IOReturn PhxBringup::loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if ((fwBufOffset & 3) || size == 0 || static_cast<uint64_t>(fwBufOffset) + size > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else {
        uint64_t mc = fwBufMc + fwBufOffset;
        uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, fwType};
        r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, out);
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 106: op 1 = SETUP_TMR (driver-managed TMR, Phoenix psp_13_0_4 is not in psp_skip_tmr), op 2 = AUTOLOAD_RLC.
IOReturn PhxBringup::pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else if (op == 1 && chip->id == RDNA3_CHIP_STRIX_POINT &&
               (!arg || arg > 0x10000000u || fbSize < 0x40000000ull)) {
        r = kIOReturnBadArgument;    // Strix: pass the LOAD_TOC response's tmr_size (0x8900000 on the 880M)
    } else if (op == 1) {
        uint32_t size = arg ? arg : 0x400000u;
        if (chip->id == RDNA3_CHIP_STRIX_POINT) {
            // PSP 14.0.0 (Linux boot_time_tmr = false) wants a TMR as large as LOAD_TOC says, far more than the 16 MiB
            // window below fw_buf: put it where Linux's allocator does, 512 MiB below the top of VRAM, 1 MiB aligned.
            size = (size + 0xFFFFFu) & ~0xFFFFFu;
            tmrOff = fbSize - 0x20000000ull;
            tmrMc = fbMcBase + tmrOff;
        } else if (size > PHX_VRAM_TMR_CAP) {
            size = PHX_VRAM_TMR_CAP;
        }
        tmrSize = size;
        // psp_gfx_cmd_setup_tmr (Linux psp_prep_tmr_cmd_buf): buf_phy_addr = TMR MC address, buf_size,
        // bitfield(virt_phy_addr=1 => 0x2), system_phy_addr = the REAL system-physical address of the TMR
        // (amdgpu_gmc_vram_pa = mc - vram_start + vram_base_offset = fbPhys + tmrOff on this UMA part).
        uint64_t tmrPa = fbPhys + tmrOff;
        uint32_t u[6] = {static_cast<uint32_t>(tmrMc), static_cast<uint32_t>(tmrMc >> 32), size, 0x2u,
                         static_cast<uint32_t>(tmrPa), static_cast<uint32_t>(tmrPa >> 32)};
        r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
        if (r == kIOReturnSuccess)
            tmrSetUp = true;
    } else if (op == 2) {
        r = pspRunCmd(PHX_PSP_GFX_CMD_AUTOLOAD_RLC, nullptr, 0, out);
    } else {
        r = kIOReturnBadArgument;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 107: submit an arbitrary GPCOM command with six caller-supplied union dwords. For bring-up
// experiments (SETUP_TMR / LOAD_TOC parameter sweeps) without rebuilding the kext.
IOReturn PhxBringup::pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, words, 6, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Polls the SMU response register for up to 2 s (Linux: usec_timeout * 20); 0 means no answer.
uint32_t PhxBringup::smuPoll() const {
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t r = rreg(PHX_SMU_RESP_REG);
        if (r)
            return r;
        IODelay(10);
    }
    return 0;
}

// One SMU 13.0.4 mailbox transaction, the sequence of Linux smu_msg_v1_send_msg().
IOReturn PhxBringup::smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    // The first message of the boot skips the pre-poll (Linux SMU_FW_INIT): nothing may have been sent yet.
    if (smuUsed && smuPoll() == 0) {
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    smuUsed = true;
    wreg(PHX_SMU_RESP_REG, 0);
    wreg(PHX_SMU_ARG_REG, arg);
    wreg(PHX_SMU_MSG_REG, msg);
    uint32_t r = smuPoll();
    *resp = r;
    *argAfter = rreg(PHX_SMU_ARG_REG);
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

bool PhxBringup::claimSession() { return OSCompareAndSwap(0, 1, &sessionOpen); }

void PhxBringup::releaseSession() { OSCompareAndSwap(1, 0, &sessionOpen); }

IOReturn PhxBringup::readRegs(uint32_t first, uint32_t count, uint32_t *out) {
    if (!mmio)
        return kIOReturnNotReady;
    uint64_t len = mmio->getLength();
    if ((static_cast<uint64_t>(first) + count) * 4 > len)
        return kIOReturnBadArgument;
    volatile uint32_t *r = reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress());
    for (uint32_t i = 0; i < count; i++)
        out[i] = r[first + i];
    return kIOReturnSuccess;
}

IOReturn PhxBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    // the sysctls go up with the first client, from user space, rather than in start(): the bootloader injects this
    // kext into the boot collection, and it starts long before the rest of BSD
    if (!gSysctlUp && OSCompareAndSwap(0, 1, &gSysctlUp)) {
        sysctl_register_oid(&sysctl__debug_phxbos_flush);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_register_oid(&sysctl__debug_phxscanout_verify);
        sysctl_register_oid(&sysctl__debug_phxscanout_state);
        sysctl_register_oid(&sysctl__debug_phxscanout_copies);
        sysctl_register_oid(&sysctl__debug_phxscanout_us_sum);
        sysctl_register_oid(&sysctl__debug_phxscanout_busy);
        sysctl_register_oid(&sysctl__debug_phxscanout_mismatch);
        sysctl_register_oid(&sysctl__debug_phxscanout_moving);
        sysctl_register_oid(&sysctl__debug_phxscanout_gpuwait_us);
        sysctl_register_oid(&sysctl__debug_phxscanout_gpulate);
        sysctl_register_oid(&sysctl__debug_phxscanout_lastbad);
        sysctl_register_oid(&sysctl__debug_phxdmcub);
        sysctl_register_oid(&sysctl__debug_phxscanout_selftest);
    }
    if (type != PHXN_UC_TYPE)
        return kIOReturnBadArgument;
    IOAccelPhxNativeClient *client = OSTypeAlloc(IOAccelPhxNativeClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- user client ----------------------------------------------------------------------------------------

bool IOAccelPhxNativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    privileged = clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task = owningTask;
    lock = IOLockAlloc();
    bos = static_cast<Bo *>(IOMallocZero(sizeof(Bo) * PHXN_MAX_BOS));
    maps = static_cast<Map *>(IOMallocZero(sizeof(Map) * kMaxMaps));
    return lock && bos && maps;
}

bool IOAccelPhxNativeClient::start(IOService *provider) {
    owner = OSDynamicCast(PhxBringup, provider);
    if (!owner)
        return false;
    // E9: several processes at once, each in its own GPUVM context (vmidAlloc); the first one still marks the session
    if (owner->claimSession())
        sessionClaimed = true;
    return IOUserClient::start(provider);
}

void IOAccelPhxNativeClient::freeAll() {
    if (!bos)
        return;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++)
        if (bos[h].md)
            boRelease(bos[h]);
}

IOReturn IOAccelPhxNativeClient::clientClose() {
    if (lock)
        IOLockLock(lock);
    freeAll();
    if (lock)
        IOLockUnlock(lock);
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (sessionClaimed && owner) {
        owner->releaseSession();
        sessionClaimed = false;
    }
    terminate();
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::free() {
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    freeAll();
    if (sessionClaimed && owner)
        owner->releaseSession();
    if (bos)
        IOFree(bos, sizeof(Bo) * PHXN_MAX_BOS);
    if (maps)
        IOFree(maps, sizeof(Map) * kMaxMaps);
    if (lock)
        IOLockFree(lock);
    IOUserClient::free();
}

IOAccelPhxNativeClient::Bo *IOAccelPhxNativeClient::findBo(uint32_t h) {
    if (h == 0 || h >= PHXN_MAX_BOS || !bos[h].md)
        return nullptr;
    return &bos[h];
}

IOReturn IOAccelPhxNativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOLockLock(lock);
    Bo *b = findBo(type);
    if (!b) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    b->md->retain();
    *memory = b->md;
    *options = 0;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

// Exact shape check, as in the contract: scalar counts and struct sizes must match.
static bool shape(const IOExternalMethodArguments *a, uint32_t sin, uint32_t sout, uint32_t stin, uint32_t stout) {
    return a->scalarInputCount == sin && a->scalarOutputCount == sout && a->structureInputSize == stin &&
           a->structureOutputSize == stout && !a->structureInputDescriptor && !a->structureOutputDescriptor;
}

IOReturn IOAccelPhxNativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector == PHXN_SEL_WAITSEQ)
        return doWaitSeq(a);

    IOLockLock(lock);
    IOReturn r;
    if (selector != PHXN_SEL_HELLO && !hello) {
        r = kIOReturnNotReady;
    } else {
        switch (selector) {
        case PHXN_SEL_HELLO:     r = doHello(a); break;
        case PHXN_SEL_QUERYINFO: r = doQueryInfo(a); break;
        case PHXN_SEL_READREGS:  r = doReadRegs(a); break;
        case PHXN_SEL_BOCREATE:  r = doBoCreate(a); break;
        case PHXN_SEL_BOFREE:    r = doBoFree(a); break;
        case PHXN_SEL_BOIMPORTHOST: r = doBoImportHost(a); break;
        case PHXN_SEL_GEMVA:     r = doGemVa(a); break;
        case PHXN_SEL_CTX:       r = doCtx(a); break;
        case PHXN_SEL_SUBMIT:    r = doSubmit(a); break;
        case PHXN_SEL_DBG_SMU:   r = doDbgSmu(a); break;
        case PHXN_SEL_DBG_PSP_RING: r = doDbgPspRing(a); break;
        case PHXN_SEL_DBG_VRAM_READ: r = doDbgVramRead(a); break;
        case PHXN_SEL_DBG_PSP_SUBMIT: r = doDbgPspSubmit(a); break;
        case PHXN_SEL_DBG_FW_STAGE: r = doDbgFwStage(a); break;
        case PHXN_SEL_DBG_PSP_LOADIPFW: r = doDbgLoadIpFw(a); break;
        case PHXN_SEL_DBG_PSP_CTL: r = doDbgPspCtl(a); break;
        case PHXN_SEL_DBG_PSP_RAW: r = doDbgPspRaw(a); break;
        case PHXN_SEL_DBG_WRITE_REG: r = doDbgWriteReg(a); break;
        case PHXN_SEL_DBG_VRAM_WRITE: r = doDbgVramWrite(a); break;
        case PHXN_SEL_DBG_DOORBELL: r = doDbgDoorbell(a); break;
        case PHXN_SEL_DBG_SYSMEM: r = doDbgSysMem(a); break;
        case PHXN_SEL_DBG_ADOPTQ: r = doDbgAdoptQueue(a); break;
        case PHXN_SEL_DBG_CLAIM: r = doDbgClaim(a); break;
        default:                 r = kIOReturnBadArgument; break;
        }
    }
    IOLockUnlock(lock);
    return r;
}

IOReturn IOAccelPhxNativeClient::doHello(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    if (a->scalarInput[1] != 0 && a->scalarInput[1] != PHXN_HELLO_F_MINOR)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] != PHXN_ABI_VERSION)
        return kIOReturnUnsupported;
    hello = true;
    a->scalarOutput[0] = PHXN_ABI_VERSION | (a->scalarInput[1] ? static_cast<uint64_t>(PHXN_ABI_MINOR) << 16 : 0);
    a->scalarOutput[1] = PHX_KEXT_BUILD;
    a->scalarOutput[2] = PHXN_MAX_IBS;
    a->scalarOutput[3] = PHXN_VMID;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doQueryInfo(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phxn_info)))
        return kIOReturnBadArgument;
    phxn_info inf;
    bzero(&inf, sizeof(inf));
    inf.abi_version = PHXN_ABI_VERSION;
    inf.kext_build = PHX_KEXT_BUILD;
    inf.reserved[2] = PHXN_ABI_MINOR;
    inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT;
    inf.vmid = PHXN_VMID;
    inf.gb_addr_config = owner->chipProfile()->gb_addr_config;
    inf.gc_version = owner->chipProfile()->gc_version;
    inf.ring_size_dw = 4096;
    inf.max_ibs = PHXN_MAX_IBS;
    inf.vram_vis_total = kVisTotal;
    inf.vram_vis_free = kVisTotal - visUsed;
    inf.vram_hi_total = kHiTotal;
    inf.vram_hi_free = kHiTotal - hiUsed;
    inf.gtt_cap = kGttCap;
    inf.gtt_used = gttUsed;
    inf.gtt_max_bo = kGttMaxBo;
    inf.seq_emitted = emitted;
    inf.seq_retired = owner->gpuReady() ? owner->gpuRetired() : emitted;
    inf.va_low_first = 0x10000;
    inf.va_low_last = 0x7FFFFFFFFFFFull;
    inf.va_high_first = 0xFFFF800000000000ull;
    inf.va_high_last = 0xFFFFFFFFFFFFFFFFull;
    inf.max_bos = PHXN_MAX_BOS;
    inf.fence_slots = PHXN_FENCE_SLOTS;
    bcopy(&inf, a->structureOutput, sizeof(inf));
    return kIOReturnSuccess;
}

static constexpr uint64_t kGoldenTscUpper = 0x16a2d;
IOReturn IOAccelPhxNativeClient::doReadRegs(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 3 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t count = a->scalarInput[1];
    if (count < 1 || count > 16 || a->scalarInput[2] != 0xFFFFFFFFull || a->structureOutputSize != count * 4 ||
        a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    // SMUIO's GOLDEN_TSC (upper 0x16a2d, lower 0x16a2e on Phoenix, SMUIO 13.0.7 base 0x16a00) is the 100 MHz counter the
    // CP stamps command buffers with: every client may read it (Mesa's AMDGPU_INFO_TIMESTAMP, which calibrates GPU
    // times against the host's; without it Metal's GPUStartTime/GPUEndTime were 0 in every app).
    const uint64_t first = a->scalarInput[0];
    const bool tsc = first >= kGoldenTscUpper && first + count <= kGoldenTscUpper + 2;
    if (!privileged && !tsc)
        return kIOReturnNotPrivileged;
    return owner->readRegs(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doBoCreate(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 4, sizeof(phxn_gem_create_in), 0))
        return kIOReturnBadArgument;
    const phxn_gem_create_in *in = static_cast<const phxn_gem_create_in *>(a->structureInput);
    uint64_t size = in->bo_size;
    if (size == 0)
        return kIOReturnBadArgument;
    size = (size + 4095) & ~4095ull;
    uint64_t align = in->alignment;
    if (align && ((align & (align - 1)) || align > (2ull << 20)))
        return kIOReturnBadArgument;
    if (align < 4096)
        align = 4096;
    const uint64_t d = in->domains, f = in->domain_flags;
    if (d == 0 || (d & ~static_cast<uint64_t>(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM)))
        return kIOReturnUnsupported;
    if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE))
        return kIOReturnUnsupported;

    uint32_t domain, bits;
    uint64_t *pool;
    if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
        if (visUsed + size <= kVisTotal) {
            domain = 4;
            bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED;
            pool = &visUsed;
        } else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
                   hiUsed + size <= kHiTotal) {
            domain = 4;
            bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED;
            pool = &hiUsed;
        } else {
            return kIOReturnNoMemory;
        }
    } else {
        if (size > kGttMaxBo || gttUsed + size > kGttCap)
            return kIOReturnNoMemory;
        domain = 2;
        bits = PHXN_PLACED_ZEROED;
        pool = &gttUsed;
    }

    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;

    // Page alignment only: the GPU maps BOs page by page, and the client picks the VA (GEMVA) with its own alignment.
    // Handed to IOBufferMemoryDescriptor, RADV's 64 KiB..2 MiB alignment made every allocation PHYSICALLY CONTIGUOUS
    // (XNU allocates an alignment above the page size with kmem_alloc_contig): once memory was fragmented, every
    // buffer of 8 MiB or more failed after a 400 ms search, and each Metal app spent ~0.8 s at launch (2026-10-21).
    (void)align;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;
    bzero(md->getBytesNoCopy(), size);
    *pool += size;
    bos[h] = Bo{md, size, domain, bits, static_cast<uint8_t *>(md->getBytesNoCopy()), false};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = domain;
    a->scalarOutput[3] = bits;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doBoFree(IOExternalMethodArguments *a) {
    if (!shape(a, 1, 0, 0, 0))
        return kIOReturnBadArgument;
    uint32_t h = static_cast<uint32_t>(a->scalarInput[0]);
    Bo *b = findBo(h);
    if (!b)
        return kIOReturnNotFound;
    for (int i = 0; i < kMaxMaps; i++)
        if (maps[i].used && maps[i].handle == h)
            maps[i].used = false;
    if (b->host)
        hostUsed -= b->size;
    else if (b->domain == 2)
        gttUsed -= b->size;
    else if (b->bits & PHXN_PLACED_HI_POOL)
        hiUsed -= b->size;
    else
        visUsed -= b->size;
    boRelease(*b);
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::boRelease(Bo &b) {
    if (b.host)
        b.md->complete();
    b.md->release();
    b = Bo{};
}

// PHXN_SEL_BOIMPORTHOST (ABI 1.9, VK_EXT_external_memory_host): a BO over the caller's own pages, wired for as long as
// it lives, GPU-mapped later through GemVa like any other. The kext reported ABI 1.12 without it, so every import
// RADV made (no-copy buffers, IOSurface textures over their surfaces' pages) failed and fell back to copies
// (2026-10-20). in: [0] host VA [1] size (both page-aligned) [2] flags 0 [3] GPU VA 0; out: [0] handle [1] size
// [2] GPU VA 0 [3] placement bits.
IOReturn IOAccelPhxNativeClient::doBoImportHost(IOExternalMethodArguments *a) {
    if (!shape(a, 4, 4, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t va = a->scalarInput[0], size = a->scalarInput[1];
    if (!va || !size || (va & 4095) || (size & 4095) || a->scalarInput[2] || a->scalarInput[3] || !task)
        return kIOReturnBadArgument;
    if (size > kHostMaxBo || hostUsed + size > kHostCap)
        return kIOReturnNoMemory;
    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;
    const IOReturn r = md->prepare();
    if (r != kIOReturnSuccess) {
        md->release();
        return r;
    }
    hostUsed += size;
    bos[h] = Bo{md, size, 2, 0, nullptr, true};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = 0;
    a->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doGemVa(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_gem_va), 0))
        return kIOReturnBadArgument;
    const phxn_gem_va *v = static_cast<const phxn_gem_va *>(a->structureInput);
    if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles ||
        v->input_fence_syncobj_handles)
        return kIOReturnBadArgument;
    if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT))
        return kIOReturnUnsupported;
    if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE)
        return kIOReturnUnsupported;
    if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP)
        return kIOReturnBadArgument;
    Bo *b = findBo(v->handle);
    if (!b)
        return kIOReturnNotFound;
    if (!vaCanonical(v->va_address))
        return kIOReturnBadArgument;
    const uint64_t va = v->va_address & kVaMask;
    if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0)
        return kIOReturnBadArgument;
    if (va < 0x10000 || va + v->map_size > (1ull << 48))
        return kIOReturnBadArgument;
    if (v->offset_in_bo + v->map_size > b->size)
        return kIOReturnBadArgument;

    if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
        int freei = -1;
        for (int i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) {
                if (freei < 0)
                    freei = i;
                continue;
            }
            if (va < maps[i].va + maps[i].size && maps[i].va < va + v->map_size)
                return kIOReturnBadArgument;
        }
        if (freei < 0)
            return kIOReturnNoMemory;
        maps[freei] = Map{true, v->handle, va, v->map_size, v->offset_in_bo, v->flags};
        if (owner->gpuReady()) {
            const IOReturn r = ensureVmid() ? owner->vmMap(vmid, va, b->md, v->offset_in_bo, v->map_size) : kIOReturnNoResources;
            if (r != kIOReturnSuccess) {
                maps[freei].used = false;
                return r;
            }
        }
        return kIOReturnSuccess;
    }
    for (int i = 0; i < kMaxMaps; i++) {
        if (maps[i].used && maps[i].handle == v->handle && maps[i].va == va && maps[i].size == v->map_size) {
            maps[i].used = false;
            if (vmid)
                owner->vmUnmap(vmid, va, v->map_size);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IOAccelPhxNativeClient::doCtx(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_ctx), sizeof(phxn_ctx)))
        return kIOReturnBadArgument;
    phxn_ctx in, out;
    bcopy(a->structureInput, &in, sizeof(in));
    bzero(&out, sizeof(out));
    if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
        uint32_t id = 1;
        while (id <= PHXN_MAX_CTX && ctx[id])
            id++;
        if (id > PHXN_MAX_CTX)
            return kIOReturnNoResources;
        ctx[id] = true;
        out.op = id;
    } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
        if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !ctx[in.ctx_id])
            return kIOReturnNotFound;
        ctx[in.ctx_id] = false;
    } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) {
        return kIOReturnBadArgument;
    }
    bcopy(&out, a->structureOutput, sizeof(out));
    return kIOReturnSuccess;
}

bool IOAccelPhxNativeClient::ibCovered(uint64_t vaCanon, uint64_t bytes) {
    uint64_t cur = vaCanon & kVaMask;
    const uint64_t end = cur + bytes;
    while (cur < end) {
        bool ok = false;
        for (int i = 0; i < kMaxMaps; i++) {
            const Map &mp = maps[i];
            if (mp.used && (mp.flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= mp.va && cur < mp.va + mp.size) {
                cur = mp.va + mp.size < end ? mp.va + mp.size : end;
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

IOReturn IOAccelPhxNativeClient::doSubmit(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 0 || a->scalarOutputCount != 1 || a->structureOutputSize != 0 ||
        a->structureInputDescriptor || a->structureInputSize < 64 ||
        a->structureInputSize > 32 + 32 * PHXN_MAX_IBS)
        return kIOReturnBadArgument;
    const uint8_t *in = static_cast<const uint8_t *>(a->structureInput);
    phxn_cs_in h;
    bcopy(in, &h, sizeof(h));
    if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS ||
        a->structureInputSize != sizeof(h) + h.num_ibs * sizeof(phxn_cs_ib) || h.reserved != 0 ||
        (h.flags & ~PHXN_CS_HAS_FENCE))
        return kIOReturnBadArgument;
    if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !ctx[h.ctx_id])
        return kIOReturnNotFound;

    for (uint32_t i = 0; i < h.num_ibs; i++) {
        phxn_cs_ib ib;
        bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
        if (ib._pad || (ib.ip_type != PHX_AMDGPU_HW_IP_GFX && ib.ip_type != PHX_AMDGPU_HW_IP_COMPUTE) || ib.ip_instance != 0 || ib.ring != 0)
            return kIOReturnBadArgument;
        if (ib.ib_bytes == 0 || (ib.ib_bytes & 3) || ib.ib_bytes > 0xFFFFFu * 4)
            return kIOReturnBadArgument;
        if (!vaCanonical(ib.va_start) || (ib.va_start & 3))
            return kIOReturnBadArgument;
        if (ib.flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE |
                        PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
            return kIOReturnUnsupported;
        if (ib.flags & ~static_cast<uint32_t>(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT |
                                              PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
            return kIOReturnBadArgument;
        if (!ibCovered(ib.va_start, ib.ib_bytes))
            return kIOReturnBadArgument;
    }

    Bo *fb = nullptr;
    if (h.flags & PHXN_CS_HAS_FENCE) {
        fb = findBo(h.fence_handle);
        if (!fb)
            return kIOReturnNotFound;
        if (!fb->kva || (h.fence_offset & 7) || static_cast<uint64_t>(h.fence_offset) + 8 > fb->size)
            return kIOReturnBadArgument;   // the kernel writes the fence: not into a host import
    } else if (h.fence_handle || h.fence_offset) {
        return kIOReturnBadArgument;
    }

    if (owner->gpuReady()) {
        // E8: real execution on the adopted compute queue, IBs run in VMID 8
        uint64_t va[PHXN_MAX_IBS];
        uint32_t dw[PHXN_MAX_IBS];
        for (uint32_t i = 0; i < h.num_ibs; i++) {
            phxn_cs_ib ib;
            bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
            va[i] = ib.va_start & kVaMask;
            dw[i] = ib.ib_bytes / 4;
        }
        // log the IBs RADV sends (first 96 dwords each) for bring-up: a client's first submissions only, as every
        // WindowServer frame dumped its IBs and the log held nothing else (2026-10-20)
        if (owner->debugEnabled() && emitted < 4) {
            for (uint32_t i = 0; i < h.num_ibs; i++) {
                for (int m = 0; m < kMaxMaps; m++) {
                    if (!maps[m].used || va[i] < maps[m].va || va[i] >= maps[m].va + maps[m].size)
                        continue;
                    Bo *bo = findBo(maps[m].handle);
                    if (!bo || !bo->kva)
                        break;
                    const uint32_t *d = reinterpret_cast<const uint32_t *>(bo->kva + maps[m].off + (va[i] - maps[m].va));
                    const uint32_t cnt = dw[i] < 96 ? dw[i] : 96;
                    for (uint32_t j = 0; j < cnt; j += 8)
                        IOLog("PhxIB %u.%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(emitted + 1), i, j,
                              d[j], j + 1 < cnt ? d[j + 1] : 0, j + 2 < cnt ? d[j + 2] : 0, j + 3 < cnt ? d[j + 3] : 0,
                              j + 4 < cnt ? d[j + 4] : 0, j + 5 < cnt ? d[j + 5] : 0, j + 6 < cnt ? d[j + 6] : 0,
                              j + 7 < cnt ? d[j + 7] : 0);
                    break;
                }
            }
        }
        flushBos();
        uint64_t seq = 0;
        phxn_cs_ib ib0;
        bcopy(in + sizeof(h), &ib0, sizeof(ib0));
        if (!ensureVmid())
            return kIOReturnNoResources;
        const IOReturn r = owner->gpuSubmit(vmid, ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, va, dw, h.num_ibs, &seq);
        if (r != kIOReturnSuccess)
            return r;
        lastSeq = seq;
        emitted = seq;   // externalMethod already holds `lock`
        if (fb) {
            pendFenceHandle = h.fence_handle;
            pendFenceOffset = h.fence_offset;
            pendFenceSeq = seq;
        }
        a->scalarOutput[0] = seq;
        return kIOReturnSuccess;
    }
    // E1: no GPU behind the call; the submission retires at once, as in the mock.
    const uint64_t seq = ++emitted;
    if (fb)
        bcopy(&seq, fb->kva + h.fence_offset, sizeof(seq));
    a->scalarOutput[0] = seq;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doDbgSmu(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 2, 0, 0) || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    switch (a->scalarInput[0]) {
    case PHX_PPSMC_TestMessage:
    case PHX_PPSMC_GetPmfwVersion:
    case PHX_PPSMC_GetDriverIfVersion:
    case PHX_PPSMC_GetEnabledSmuFeatures:
    case PHX_PPSMC_AllowGfxOff:
    case PHX_PPSMC_DisallowGfxOff:
    case PHX_PPSMC_EnableGfxImu:
        break;
    default:
        return kIOReturnNotPermitted;
    }
    uint32_t resp = 0, after = 0;
    IOReturn r = owner->smuMessage(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                   &resp, &after);
    a->scalarOutput[0] = resp;
    a->scalarOutput[1] = after;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRing(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRing(static_cast<uint32_t>(a->scalarInput[0]), a->scalarInput[1], out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramRead(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t off = a->scalarInput[0], count = a->scalarInput[1];
    if (count < 1 || count > 256 || off > 0xFFFFFFFFull || a->structureOutputSize != count * 4)
        return kIOReturnBadArgument;
    return owner->vramRead(static_cast<uint32_t>(off), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doDbgPspSubmit(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspSubmit(static_cast<uint32_t>(a->scalarInput[0]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgFwStage(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarOutputCount != 0 || a->structureOutputSize != 0)
        return kIOReturnBadArgument;
    if (a->structureInputDescriptor || !a->structureInput || a->structureInputSize == 0 ||
        a->structureInputSize > 4096 || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->fwStage(static_cast<uint32_t>(a->scalarInput[0]), a->structureInput, a->structureInputSize);
}

IOReturn IOAccelPhxNativeClient::doDbgLoadIpFw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull ||
        a->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->loadIpFw(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                 static_cast<uint32_t>(a->scalarInput[2]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspCtl(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspCtl(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRaw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 7, 4, 0, 0))
        return kIOReturnBadArgument;
    uint32_t w[6];
    for (int i = 0; i < 6; i++)
        w[i] = static_cast<uint32_t>(a->scalarInput[i + 1]);
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRaw(static_cast<uint32_t>(a->scalarInput[0]), w, out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgWriteReg(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 1, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint32_t rb = 0;
    IOReturn r = owner->writeReg(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), &rb);
    a->scalarOutput[0] = rb;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramWrite(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 0, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->vramWrite(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]));
}

IOReturn IOAccelPhxNativeClient::doDbgDoorbell(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->doorbellWrite(a->scalarInput[0], a->scalarInput[1] | (a->scalarInput[2] << 32));
}

IOReturn IOAccelPhxNativeClient::doDbgSysMem(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 2, 0, 0))
        return kIOReturnBadArgument;
    return owner->sysMem(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], &a->scalarOutput[0], &a->scalarOutput[1]);
}

IOReturn IOAccelPhxNativeClient::doDbgAdoptQueue(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->adoptQueue(a->scalarInput);
}

IOReturn IOAccelPhxNativeClient::doDbgClaim(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 0, 0, 0) || a->scalarInput[0] != 1)
        return kIOReturnBadArgument;
    return owner->claimAccel();
}

// Write back and invalidate the CPU caches over every BO: CPU-written data must be in memory before the GPU reads
// it, and the CPU must not read stale lines after the GPU wrote. Coarse: WindowServer's BOs (full-screen surfaces
// among them) made this its hottest loop, hence debug.phxbos_flush.
void IOAccelPhxNativeClient::flushBos() {
    const int mode = gBosFlush;
    if (!mode)
        return;
    uint64_t bytes = 0;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++) {
        if (!bos[h].kva)   // host imports: the CPU's own pages, which the GPU reads and writes snooped
            continue;
        const uint8_t *p = bos[h].kva;
        if (mode == 1) {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
        } else {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflush (%0)" ::"r"(p + o) : "memory");
        }
        bytes += bos[h].size;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    OSAddAtomic64((SInt64)bytes, &gBosFlushBytes);
}

IOReturn IOAccelPhxNativeClient::doWaitSeq(IOExternalMethodArguments *a) {
    if (!hello)
        return kIOReturnNotReady;
    if (!shape(a, 3, 3, 0, 0))
        return kIOReturnBadArgument;
    IOLockLock(lock);
    uint64_t target = a->scalarInput[0];
    const uint64_t em = emitted;
    IOLockUnlock(lock);
    if (target == PHXN_SEQ_LAST)
        target = em;
    if (target > em)
        return kIOReturnBadArgument;
    if (owner->gpuReady()) {
        // E8: poll the RELEASE_MEM fence the adopted queue writes after each submission
        uint64_t timeout = a->scalarInput[1];
        if (timeout > PHXN_WAIT_CAP_NS)
            timeout = PHXN_WAIT_CAP_NS;
        bool done = owner->gpuSeqDone(target);
        for (uint64_t waited = 0; !done && waited < timeout; waited += 100000) {
            IODelay(100);
            done = owner->gpuSeqDone(target);
        }
        const uint64_t retired = owner->gpuRetired();   // still "all up to here", for the client's bookkeeping
        if (done) {
            // drop CPU cache lines so the CPU sees what the GPU wrote; under the lock, as another thread of the same
            // client (WindowServer) freed a BO mid-flush and the walk dereferenced its NULL descriptor (panic 2026-10-20)
            IOLockLock(lock);
            flushBos();
            if (pendFenceSeq && owner->gpuSeqDone(pendFenceSeq)) {
                Bo *fb = findBo(pendFenceHandle);
                if (fb && fb->kva && pendFenceOffset + 8 <= fb->size)
                    bcopy(&pendFenceSeq, fb->kva + pendFenceOffset, 8);
                pendFenceSeq = 0;
            }
            IOLockUnlock(lock);
        }
        a->scalarOutput[0] = !done;
        a->scalarOutput[1] = retired;
        a->scalarOutput[2] = em;
        return kIOReturnSuccess;
    }
    a->scalarOutput[0] = 0;   // not busy: E1 retires every submission at once
    a->scalarOutput[1] = em;
    a->scalarOutput[2] = em;
    return kIOReturnSuccess;
}
