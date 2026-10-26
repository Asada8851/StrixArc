// mesboot: Phoenix (GC 11.0.1) GART + MES bring-up experiments, driven through PhxBringup debug selectors.
// Register addresses below are written in Phoenix (GC 11.0) terms; rd()/wr() move the GCVM ones to their GC 11.5
// place on Strix Point (src/amd/rdna3_chip.h). The chip comes from the GPU's PCI device ID, or MESBOOT_CHIP.
// build: clang -O2 -I../../src/amd -o mesboot mesboot.c -framework IOKit -framework CoreFoundation ; run as root (phxdebug=1)
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "rdna3_chip.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "mes_v11_api_def.h"

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_DBG_VRAM_READ 102
#define SEL_DBG_WRITE_REG 108
#define SEL_DBG_VRAM_WRITE 109
#define SEL_DBG_DOORBELL 110
#define SEL_DBG_SYSMEM 111
#define SEL_DBG_ADOPTQ 112

static io_connect_t conn;
static const struct rdna3_chip *chip;
static uint32_t hdp_flush_dword;   // the kext's HDP flush register ("phx-hdp-flush-dword"), 0 when it has none

// PCI device ID of the GPU PhxBringup is attached to ("device-id" on the IOPCIDevice, little-endian data).
static uint16_t pci_device_id(io_service_t svc)
{
   uint16_t id = 0;
   CFTypeRef p = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, CFSTR("device-id"), kCFAllocatorDefault,
                                                 kIORegistryIterateRecursively | kIORegistryIterateParents);
   if (p && CFGetTypeID(p) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)p) >= 2) {
      const UInt8 *b = CFDataGetBytePtr((CFDataRef)p);
      id = (uint16_t)(b[0] | (b[1] << 8));
   }
   if (p) CFRelease(p);
   return id;
}

static void pick_chip(io_service_t svc)
{
   const char *e = getenv("MESBOOT_CHIP");
   if (e && !strcmp(e, "phoenix")) chip = &rdna3_chip_phoenix;
   else if (e && !strcmp(e, "strix-point")) chip = &rdna3_chip_strix_point;
   else chip = rdna3_chip_for_pci(pci_device_id(svc));
   if (!chip) { fprintf(stderr, "mesboot: unknown GPU 0x%04x (set MESBOOT_CHIP=phoenix|strix-point)\n", pci_device_id(svc)); exit(1); }
   if (!chip->verified_on_hardware) fprintf(stderr, "mesboot: %s has never been brought up on hardware\n", chip->name);
}

// Phoenix-terms address -> this chip's address; a GCVM register the table does not know stops the run.
static uint32_t xl(uint32_t reg)
{
   uint32_t r;
   if (!rdna3_gcvm_translate(chip, reg, &r)) {
      fprintf(stderr, "mesboot: no %s address for GCVM register 0x%x, stopping\n", chip->name, reg);
      exit(1);
   }
   return r;
}

static int open_kext(void)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!svc) { fprintf(stderr, "no PhxBringup\n"); return -1; }
   pick_chip(svc);
   CFTypeRef hp = IORegistryEntryCreateCFProperty(svc, CFSTR("phx-hdp-flush-dword"), kCFAllocatorDefault, 0);
   if (hp && CFGetTypeID(hp) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)hp, kCFNumberSInt32Type, &hdp_flush_dword);
   if (hp) CFRelease(hp);
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), UC_TYPE, &conn);
   uint64_t hin[2] = {1, 0}, hout[4]; uint32_t hn = 4;
   if (kr == 0) kr = IOConnectCallScalarMethod(conn, SEL_HELLO, hin, 2, hout, &hn);
   if (kr) { fprintf(stderr, "open: 0x%x\n", kr); return -1; }
   return 0;
}
static uint32_t rd(uint32_t reg)
{
   uint64_t in[3] = {xl(reg), 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
   IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
   return v;
}
static uint32_t wr(uint32_t reg, uint32_t val)
{
   uint64_t in[2] = {xl(reg), val}, out[1] = {0}; uint32_t n = 1;
   IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
   return (uint32_t)out[0];
}
static kern_return_t db(uint64_t byteOff, uint64_t v)
{
   uint64_t in[3] = {byteOff, v & 0xffffffffu, v >> 32};
   return IOConnectCallScalarMethod(conn, SEL_DBG_DOORBELL, in, 3, NULL, NULL);
}
static kern_return_t sm(uint64_t op, uint64_t a, uint64_t b, uint64_t *o0, uint64_t *o1)
{
   uint64_t in[3] = {op, a, b}, out[2] = {0, 0}; uint32_t n = 2;
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_SYSMEM, in, 3, out, &n);
   if (o0) *o0 = out[0];
   if (o1) *o1 = out[1];
   return kr;
}
static void vw(uint64_t off, uint32_t v)
{
   uint64_t in[2] = {off, v};
   IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
}
static uint32_t vr(uint64_t off)
{
   uint64_t in[2] = {off, 4}; uint32_t buf[4] = {0}; size_t sz = sizeof(buf);
   IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, buf, &sz);
   return buf[0];
}


#define VRAM_MC 0x8000000000ull          // MC address of VRAM offset 0
#define GART_VA 0x7FFF00000000ull            // VA = GART_VA + vram offset (first 128 MiB)
#define GART_PT 0x2400000ull             // flat PTE table (VRAM offset)
#define GART_N 32768
// System physical address of VRAM offset 0 (GCMC_VM_FB_OFFSET << 24): 0x6A0000000 on the Firebat U6C, read from the
// GPU elsewhere (set in main once the kext is open).
static uint64_t fb_pa = 0x6A0000000ull;
#define FB_PA fb_pa

#define SEG0(o) (0x1260u + (o))
#define SEG1(o) (0xA000u + (o))
// VRAM layout of the experiment (offsets), all inside the GART window
#define KIQ_RING 0x2500000ull            // 4 KiB ring
#define KIQ_WB   0x2501000ull            // +0 rptr report, +16 wptr poll
#define KIQ_MQD  0x2502000ull            // 2 KiB
#define KIQ_EOP  0x2503000ull            // 2 KiB
#define SCH_RING 0x2510000ull
#define SCH_WB   0x2511000ull
#define SCH_MQD  0x2512000ull
#define SCH_EOP  0x2513000ull
#define TESTBUF  0x2520000ull
#define MES_RES1 0x2590000ull            // MES resource_1: the cleaner shader fence page (SET_HW_RESOURCES_1)
#define DUMMY_PG 0x2800000ull            // default/dummy page (Strix Point; Phoenix uses the Linux trace values); free: rings end below 0x2702000

static int env_on(const char *n) { const char *v = getenv(n); return v && *v && *v != '0'; }
static int use_mc;
#define VA(off) ((use_mc ? VRAM_MC : GART_VA) + (off))
#define MC(off) (VRAM_MC + (off))

// The UMA carve-out is DRAM nobody clears at boot: what was never written holds a near-constant power-up pattern
// (try8..try11's fault addresses 0x1:6b0857ca, 0x1:6b0855c8, 0x1:eb2bd5c6, 0x5:6b6857c0 are that pattern). Linux
// hands the GPU only zeroed memory (the WB slots MES gets as g_sch_ctx / query_status_fence, memset EOP buffers, a
// zeroed dummy page); mesboot zeroed only the parts it wrote itself. Clear the whole bring-up region and the dummy page.
static void vzero(uint64_t off, uint64_t bytes)
{
   for (uint64_t i = 0; i < bytes; i += 4) vw(off + i, 0);
}
static void gart(void)
{
   if (!env_on("NO_VZERO")) {
      vzero(0x2500000ull, 0x100000ull);   // KIQ, MES, compute, gfx queues, TESTBUF, fences, MES resource_1
      vzero(DUMMY_PG, 0x1000);
      printf("gart: VRAM 0x2500000..0x25fffff and the dummy page cleared (%s)\n",
             vr(0x25ffffc) == 0 && vr(DUMMY_PG + 0xffc) == 0 ? "verified" : "READBACK NOT ZERO");
   }
   for (uint64_t i = 0; i < GART_N; i++) {
      uint64_t pte = (FB_PA + i * 4096) | ((getenv("PTE_SYS") && *getenv("PTE_SYS")) ? 0x77 : 0x71);      // VALID | EXEC | READ | WRITE, local FB
      vw(GART_PT + i * 8, (uint32_t)pte);
      vw(GART_PT + i * 8 + 4, (uint32_t)(pte >> 32));
   }
   wr(SEG0(0x1713), (uint32_t)(GART_VA >> 12)); wr(SEG0(0x1714), (uint32_t)(GART_VA >> 44));
   wr(SEG0(0x1733), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 12)); wr(SEG0(0x1734), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 44));
   wr(SEG0(0x16f3), (uint32_t)((FB_PA + GART_PT) | 1)); wr(SEG0(0x16f4), (uint32_t)((FB_PA + GART_PT) >> 32));   // PDB base = physical (FB_PA + vram offset), as the Linux trace
   // system aperture / AGP / defaults exactly as the Linux trace (gfxhub_v3_0_init_system_aperture_regs)
   wr(SEG0(0x167c), 0); wr(SEG0(0x167b), 0x00ffffff); wr(SEG0(0x167a), 0);      // AGP_BASE/BOT/TOP (AGP off)
   if (chip->id == RDNA3_CHIP_PHOENIX) {
      wr(SEG0(0x167d), 0x00200000); wr(SEG0(0x167e), 0x00205fff);               // SYSTEM_APERTURE LOW/HIGH = VRAM MC range
      wr(SEG0(0x15a8), 0x006a0f9f); wr(SEG0(0x15a9), 0);                        // SYSTEM_APERTURE_DEFAULT_ADDR LSB/MSB
      wr(SEG0(0x15cb), 0x0000a23c); wr(SEG0(0x15cc), 0);                        // L2 protection fault default addr
   } else {
      // The Firebat values above are that machine's Linux trace (its VRAM range, scratch page and dummy page). Here
      // they are computed the way gfxhub_v11_5_0_init_system_aperture_regs does, with one page of the experiment's
      // VRAM (DUMMY_PG, nothing else uses it) standing in for both the scratch and the dummy page.
      uint64_t fb_start = (uint64_t)(rd(SEG0(0x1678)) & 0xffffff) << 24;           // GCMC_VM_FB_LOCATION_BASE
      uint64_t fb_end = ((uint64_t)(rd(SEG0(0x1679)) & 0xffffff) << 24) | 0xffffff; // GCMC_VM_FB_LOCATION_TOP
      uint64_t dummy = FB_PA + DUMMY_PG;
      wr(SEG0(0x167d), (uint32_t)(fb_start >> 18)); wr(SEG0(0x167e), (uint32_t)(fb_end >> 18));
      wr(SEG0(0x15a8), (uint32_t)(dummy >> 12)); wr(SEG0(0x15a9), (uint32_t)(dummy >> 44));
      wr(SEG0(0x15cb), (uint32_t)(dummy >> 12)); wr(SEG0(0x15cc), (uint32_t)(dummy >> 44));
      printf("gart: fb 0x%llx..0x%llx pa 0x%llx dummy 0x%llx\n", (unsigned long long)fb_start,
             (unsigned long long)fb_end, (unsigned long long)FB_PA, (unsigned long long)dummy);
   }
   wr(SEG0(0x15c5), 0x00060000);                                                // L2_PROTECTION_FAULT_CNTL2
   // L2_PROTECTION_FAULT_CNTL. 0x3ffffffc sends a faulting access to the dummy page (Linux's setting, also on the 880M
   // under Linux); 0x2 records faults without redirecting them, which stalls the faulting client. Strix Point defaults to
   // the redirect: its gfx CP makes one read of an unmapped address at the first IB of every new VMID context (try8..
   // try13, cause not found) and without the redirect that stalls the gfx queue (try7) - phxboot's full.sh passes an
   // empty FAULT_DEFAULT. FAULT_DEFAULT=0/1 overrides; Phoenix keeps its old default (off).
   const char *fde = getenv("FAULT_DEFAULT");
   const int fd = (fde && *fde) ? *fde != '0' : chip->id == RDNA3_CHIP_STRIX_POINT;
   wr(SEG0(0x15c4), fd ? 0x3ffffffc : 0x00000002);
   wr(SEG0(0x15ce), 0xffffffff); wr(SEG0(0x15cf), 0x0000000f); wr(SEG0(0x15d0), 0);  // identity aperture LOW/HIGH (disabled)
   wr(SEG0(0x167f), 0x1859);                 // GCMC_VM_MX_L1_TLB_CNTL
   // GCVM L2 exactly as the working Linux driver programs it (regtrace, 1.558983..)
   wr(SEG0(0x15bc), 0x00080e01);             // GCVM_L2_CNTL
   wr(SEG0(0x15bd), 0x00000003);             // GCVM_L2_CNTL2 (invalidate all L1 TLBs + L2)
   wr(SEG0(0x15be), 0x80130009);             // GCVM_L2_CNTL3
   wr(SEG0(0x15d4), 0x00000001);             // GCVM_L2_CNTL4
   wr(SEG0(0x15da), 0x00003fe0);             // GCVM_L2_CNTL5
   wr(SEG0(0x1688), 0x01fffe01);             // GCVM_CONTEXT0_CNTL: enable, depth 0
   // gfxhub_v3_0_program_invalidation: address range of every invalidation engine = everything (was 0 => flushes did nothing)
   for (int e = 0; e < 18; e++) { wr(0x292f + 2 * e, 0xffffffff); wr(0x2930 + 2 * e, 0x0000001f); }
   // contexts 1..15 (VMIDs) as the Linux driver enables them
   for (int c = 1; c < 16; c++) {
      wr(0x28e8 + c, 0x01fffe07);
      wr(0x2973 + 2 * c, 0); wr(0x2974 + 2 * c, 0);
      wr(0x2993 + 2 * c, 0xffffffff); wr(0x2994 + 2 * c, 0x0000000f);
   }
   // SH_MEM_CONFIG / SH_MEM_BASES per VMID as in gfx_v11_0_constants_init (trace 1.559162..)
   for (int v = 0; v < 16; v++) {
      wr(SEG1(0x900), v << 4);
      wr(0xa9e4, 0x0000c00c);
      if (v) wr(0xa9e3, v < 8 ? 0x20001000 : 0x00010002);
   }
   wr(SEG1(0x900), 0);
   wr(SEG0(0x1e1f), rd(SEG0(0x1e1f)) | 0x8000);   // CP_DEBUG.CPG_UTCL1_ERROR_HALT_DISABLE (linux sets it with the gfxhub)
   wr(SEG0(0x16ab + 17), 0x00F80001);          // GCVM_INVALIDATE_ENG17_REQ (the engine amdgpu uses for gmc flushes)
   usleep(2000);
   printf("gart: ack17=0x%x ctx0=0x%x fault redirect %s\n", rd(SEG0(0x16bd + 17)), rd(SEG0(0x1688)),
          rd(SEG0(0x15c4)) == 0x3ffffffc ? "on (dummy page)" : "off");
}
static void grbm_select(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
   wr(SEG1(0x900), pipe | (me << 2) | (vmid << 4) | (queue << 8));
}
static void mes_start(void)
{
   // uc start from fw headers: lo=0xF0005000? taken from the header words (same for sched + kiq)
   const uint64_t uc = 0xFFFFFFFFF0005000ull >> 2;
   wr(SEG1(0x2807), (1u << 16) | (1u << 17));                // PIPE0/1 RESET
   for (int p = 0; p < 2; p++) {
      grbm_select(3, p, 0, 0);
      wr(SEG1(0x2800), (uint32_t)uc);
      wr(SEG1(0x289d), (uint32_t)(uc >> 32));
   }
   grbm_select(0, 0, 0, 0);
   wr(SEG1(0x2807), (1u << 26) | (1u << 27));                // PIPE0/1 ACTIVE
   usleep(2000);
   printf("mes: CP_MES_CNTL=0x%x\n", rd(SEG1(0x2807)));
}
static void mqd_build(uint64_t mqd, uint64_t ring, uint64_t wb, uint64_t eop, int doorbell, uint32_t db_index)
{
   for (int i = 0; i < 512; i++) vw(mqd + i * 4, 0);
   #define M(i, v) vw(mqd + (i) * 4, (uint32_t)(v))
   M(0, 0xC0310800);
   M(11, 1);                                                  // compute_pipelinestat_enable
   M(23, 0xffffffff); M(24, 0xffffffff); M(26, 0xffffffff); M(27, 0xffffffff);
   M(32, 7);                                                  // compute_misc_reserved
   M(128, VA(mqd) & 0xfffffffc); M(129, VA(mqd) >> 32);
   M(130, 1);                                                 // hqd_active
   M(131, 0);                                                 // hqd_vmid
   M(132, (0x0be05501u & ~(0xffu << 8)) | (0x55u << 8));      // persistent_state, PRELOAD_SIZE=0x55
   M(135, 0);
   M(136, VA(ring) >> 8); M(137, VA(ring) >> 40);
   M(138, 0);
   M(139, VA(wb) & 0xfffffffc); M(140, (VA(wb) >> 32) & 0xffff);
   M(141, VA(wb + 16) & 0xfffffff8); M(142, (VA(wb + 16) >> 32) & 0xffff);
   uint32_t db = 0;
   if (doorbell) db = (db_index << 2) | (1u << 30);
   M(143, db);
   // PQ_CONTROL: default 0x00308509; QUEUE_SIZE=log2(4096/4)-1=9, RPTR_BLOCK_SIZE=(log2(4096/4)-1)<<8 (=9<<8), UNORD=1,PRIV=1,KMD=1,NO_UPDATE_RPTR=1
   uint32_t pq = 0x00308509;
   pq = (pq & ~0xffu) | 9u;
   pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27);
   pq &= ~(1u << 29);
   M(145, pq);
   M(149, 0x00300000); M(150, 0);
   M(162, 0x00000100);                                        // cp_mqd_control (VMID 0)
   M(165, VA(eop) >> 8); M(166, VA(eop) >> 40);
   M(167, 8);                                                 // EOP_SIZE = log2(2048/4)-1
   M(182, 0); M(183, 0);
   #undef M
}
static void hqd_program(uint32_t me, uint32_t pipe, uint64_t mqd, uint64_t ring, uint64_t wb, uint32_t pq, uint32_t db)
{
   grbm_select(me, pipe, 0, 0);
   wr(SEG0(0x1fab), 0);                                       // deactivate a previously programmed queue
   wr(SEG0(0x1fb3), 0); wr(SEG0(0x323f), 0); wr(SEG0(0x3240), 0);
   wr(SEG0(0x1fac), 0);                                       // HQD_VMID
   wr(SEG0(0x1fb8), 0);                                       // doorbell off while programming
   wr(SEG0(0x1fa9), (uint32_t)(VA(mqd) & 0xfffffffc)); wr(SEG0(0x1faa), (uint32_t)(VA(mqd) >> 32));
   wr(SEG0(0x1fcb), 0);
   wr(SEG0(0x1fb1), (uint32_t)(VA(ring) >> 8)); wr(SEG0(0x1fb2), (uint32_t)(VA(ring) >> 40));
   wr(SEG0(0x1fb4), (uint32_t)(VA(wb) & 0xfffffffc)); wr(SEG0(0x1fb5), (uint32_t)((VA(wb) >> 32) & 0xffff));
   wr(SEG0(0x1fba), pq);
   wr(SEG0(0x1fb6), (uint32_t)(VA(wb + 16) & 0xfffffff8)); wr(SEG0(0x1fb7), (uint32_t)((VA(wb + 16) >> 32) & 0xffff));
   wr(SEG0(0x1fb8), db);
   wr(SEG0(0x1fad), (0x0be05501u & ~(0xffu << 8)) | (0x55u << 8));
   wr(SEG0(0x1fab), 1);                                       // HQD_ACTIVE
   grbm_select(0, 0, 0, 0);
}
static int use_db;
static void kiq_test(void)
{
   // RLC_CP_SCHEDULERS: KIQ = me3 pipe1 queue0
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, use_db, 0x18);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, pq, use_db ? ((0x18u << 2) | (1u << 30)) : 0);
   // ring: WRITE_DATA to TESTBUF (VA) magic
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   vw(KIQ_RING + 0, 0xC0033700); vw(KIQ_RING + 4, 0x00100500);
   vw(KIQ_RING + 8, (uint32_t)VA(TESTBUF)); vw(KIQ_RING + 12, (uint32_t)(VA(TESTBUF) >> 32));
   vw(KIQ_RING + 16, 0x4B1D0001);
   // kick: MMIO wptr (doorbell disabled) under GRBM select me3/pipe1
   if (use_db) {
      vw(KIQ_WB + 16, 5); vw(KIQ_WB + 20, 0);
      kern_return_t kr = db(0x18 * 4, 5);
      printf("doorbell write kr=0x%x\n", kr);
   } else {
      grbm_select(3, 1, 0, 0);
      wr(SEG0(0x323f), 5); wr(SEG0(0x3240), 0);
   }
   usleep(100000);
   // second test: SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF (MMIO side effect, no memory involved)
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(KIQ_RING + 20, 0xC0017900); vw(KIQ_RING + 24, 0x40); vw(KIQ_RING + 28, 0xDEADBEEF);
   if (use_db) { vw(KIQ_WB + 16, 8); db(0x18 * 4, 8); }
   usleep(100000);
   printf("scratch0=0x%08x (expect deadbeef)\n", rd(SEG1(0x2040)));
   // map the SCHED pipe's queue through the KIQ: MAP_QUEUES(me=2 eng_sel=5 pipe0 queue0)
   mqd_build(SCH_MQD, SCH_RING, SCH_WB, SCH_EOP, 1, 0x16);
   for (int i = 0; i < 4; i++) { vw(SCH_WB + i * 4, 0); vw(SCH_WB + 16 + i * 4, 0); }
   vw(KIQ_RING + 32, 0xC0055E00);
   vw(KIQ_RING + 36, (2u << 18) | (5u << 26) | (1u << 29));
   vw(KIQ_RING + 40, 0x16u << 2);
   vw(KIQ_RING + 44, (uint32_t)VA(SCH_MQD)); vw(KIQ_RING + 48, (uint32_t)(VA(SCH_MQD) >> 32));
   vw(KIQ_RING + 52, (uint32_t)VA(SCH_WB + 16)); vw(KIQ_RING + 56, (uint32_t)(VA(SCH_WB + 16) >> 32));
   if (use_db) { vw(KIQ_WB + 16, 15); db(0x18 * 4, 15); }
   usleep(200000);
   grbm_select(3, 0, 0, 0);
   printf("sched hqd: active=%u pq_base=0x%x rptr=%u dbctl=0x%x\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb1)), rd(SEG0(0x1fb3)), rd(SEG0(0x1fb8)));
   grbm_select(3, 1, 0, 0);
   printf("kiq rptr after map=%u\n", rd(SEG0(0x1fb3)));
   grbm_select(3, 1, 0, 0);
   uint32_t rp = rd(SEG0(0x1fb3)), act = rd(SEG0(0x1fab)), wp = rd(SEG0(0x323f));
   grbm_select(0, 0, 0, 0);
   printf("kiq: active=%u rptr=%u wptr=%u target=0x%08x wb_rptr=0x%08x\n", act, rp, wp, vr(TESTBUF), vr(KIQ_WB));
}

static void mesdump(void)
{
   for (int p = 0; p < 2; p++) {
      grbm_select(3, p, 0, 0);
      printf("pipe%d: IC_BASE=%08x:%08x MDBASE=%08x:%08x PC_START=%08x INSTR_PNTR=%08x HDR=%08x GP0=%08x GP1=%08x MIBOUND=%08x\n", p,
             rd(SEG1(0x5851)), rd(SEG1(0x5850)), rd(SEG1(0x5855)), rd(SEG1(0x5854)), rd(SEG1(0x2800)),
             rd(SEG1(0x2813)), rd(SEG1(0x280d)), rd(SEG1(0x2843)), rd(SEG1(0x2845)), rd(SEG1(0x585b)));
   }
   grbm_select(0, 0, 0, 0);
   printf("CP_MES_CNTL=%08x RLC_CP_SCHEDULERS=%08x\n", rd(SEG1(0x2807)), rd(SEG1(0x98a)));
}

// ---- MES scheduler pipe (pipe0) driven by direct HQD programming + API ring ----
static uint32_t sch_wptr;      // in dwords
static uint64_t sch_seq;

static int skip_hqd;
static void sched_init(void)
{
   if (!skip_hqd) mqd_build(SCH_MQD, SCH_RING, SCH_WB, SCH_EOP, 1, 0x16);
   for (int i = 0; i < 1024; i++) vw(SCH_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(SCH_WB + i * 4, 0);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   if (!skip_hqd) hqd_program(3, 0, SCH_MQD, SCH_RING, SCH_WB, pq, (0x16u << 2) | (1u << 30));
   sch_wptr = 0; sch_seq = 0;
   grbm_select(3, 0, 0, 0);
   printf("sched: hqd active=%u pq_base=0x%x\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb1)));
   grbm_select(0, 0, 0, 0);
}
static int mes_submit(void *pkt, size_t size, size_t api_status_off, const char *what)
{
   uint32_t *dw = pkt;
   uint64_t status_va = VA(SCH_WB + 64);
   for (int i = 0; i < 4; i++) vw(SCH_WB + 64 + i * 4, 0);
   *(uint64_t *)((char *)pkt + api_status_off) = status_va;
   *(uint64_t *)((char *)pkt + api_status_off + 8) = 1;
   union MESAPI__QUERY_MES_STATUS q;
   memset(&q, 0, sizeof(q));
   q.header.type = MES_API_TYPE_SCHEDULER;
   q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
   q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   sch_seq++;
   q.api_status.api_completion_fence_addr = VA(SCH_WB + 32);
   q.api_status.api_completion_fence_value = sch_seq;
   uint32_t *qd = (uint32_t *)&q;
   for (size_t i = 0; i < size / 4; i++) vw(SCH_RING + (sch_wptr + i) * 4, dw[i]);
   for (size_t i = 0; i < sizeof(q) / 4; i++) vw(SCH_RING + (sch_wptr + size / 4 + i) * 4, qd[i]);
   sch_wptr += (uint32_t)((size + sizeof(q)) / 4);
   vw(SCH_WB + 16, sch_wptr); vw(SCH_WB + 20, 0);          // wptr shadow (polled by MES) before the doorbell
   db(0x16 * 4, sch_wptr);
   for (int t = 0; t < 400; t++) {
      usleep(5000);
      if (vr(SCH_WB + 32) == (uint32_t)sch_seq) break;
   }
   grbm_select(3, 0, 0, 0);
   printf("  sched hqd: rptr=%u wptr_lo=%u dbctl=0x%08x active=%u\n", rd(SEG0(0x1fb3)), rd(SEG0(0x323f)), rd(SEG0(0x1fb8)), rd(SEG0(0x1fab)));
   grbm_select(0, 0, 0, 0);
   uint32_t f = vr(SCH_WB + 32), st = vr(SCH_WB + 64);
   printf("mes %s: fence=%u (want %llu) status=%u -> %s\n", what, f, (unsigned long long)sch_seq, st, (f == sch_seq && st) ? "OK" : "TIMEOUT");
   return !(f == sch_seq && st);
}
#define CMP_RING 0x2540000ull
#define CMP_WB   0x2541000ull
#define CMP_MQD  0x2542000ull
#define CMP_EOP  0x2543000ull
static void vm8_test(void);
static void gfx_via_mes(void);
static void compute_via_mes(void)
{
   for (int i = 0; i < 1024; i++) vw(CMP_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(CMP_WB + i * 4, 0);
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   mqd_build(CMP_MQD, CMP_RING, CMP_WB, CMP_EOP, 1, 6);
   union MESAPI__ADD_QUEUE a;
   memset(&a, 0, sizeof(a));
   a.header.type = MES_API_TYPE_SCHEDULER;
   a.header.opcode = MES_SCH_API_ADD_QUEUE;
   a.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   a.pipe_id = 0; a.queue_id = 0;
   a.doorbell_offset = 6;
   a.mqd_addr = VA(CMP_MQD);
   a.wptr_addr = VA(CMP_WB + 16);
   a.queue_type = MES_QUEUE_TYPE_COMPUTE;
   a.map_legacy_kq = 1;
   if (mes_submit(&a, sizeof(a), offsetof(union MESAPI__ADD_QUEUE, api_status), "ADD_QUEUE(compute, map_legacy_kq)")) return;
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(CMP_RING + 0, 0xC0017900); vw(CMP_RING + 4, 0x40); vw(CMP_RING + 8, 0xDEADBEEF);
   vw(CMP_RING + 12, 0xC0033700); vw(CMP_RING + 16, 0x00100500);
   vw(CMP_RING + 20, (uint32_t)VA(TESTBUF)); vw(CMP_RING + 24, (uint32_t)(VA(TESTBUF) >> 32)); vw(CMP_RING + 28, 0xFEEDF00D);
   vw(CMP_WB + 16, 8); vw(CMP_WB + 20, 0);
   db(6 * 4, 8);
   for (int i = 0; i < 50; i++) { usleep(10000); if (rd(SEG1(0x2040)) == 0xDEADBEEF && vr(TESTBUF) == 0xFEEDF00D) break; }
   grbm_select(1, 0, 0, 0);
   printf("compute: scratch0=0x%08x (want deadbeef) testbuf=0x%08x (want feedf00d) hqd_active=%u rptr=%u\n", rd(SEG1(0x2040)), vr(TESTBUF), rd(SEG0(0x1fab)), rd(SEG0(0x1fb3)));
   grbm_select(0, 0, 0, 0);
   if (env_on("ADOPT")) {   // hand the working compute queue to the kext: SUBMIT from RADV goes here from now on
      uint64_t in[6] = {CMP_RING, 1024, CMP_WB, 6 * 4, 8, 0x2550000};
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTQ, in, 6, NULL, NULL);
      printf("adopt queue -> kr 0x%x\n", kr);
      return;
   }
   if (env_on("VM8")) { vm8_test(); return; }
   if (getenv("MESBOOT_VMT") && *getenv("MESBOOT_VMT")) {
      // GPU write into SYSTEM memory through a context0 (GART) mapping, PTE SYSTEM|SNOOPED
      uint64_t phys = 0, len = 0, val = 0;
      kern_return_t kr = sm(1, 1 << 20, 0, &phys, &len);
      printf("sysmem alloc kr=0x%x phys=0x%llx len=0x%llx\n", kr, (unsigned long long)phys, (unsigned long long)len);
      if (kr) return;
      if (env_on("AGPTEST")) {   // can the GPU write system memory at all? identity AGP aperture MC==physical, no page tables
         wr(SEG0(0x167c), 0); wr(SEG0(0x167b), 0); wr(SEG0(0x167a), 0x000001ff);       // AGP BASE=0, BOT=0, TOP=0x1ff (8 GiB, 16 MiB units)
         if (env_on("CTX0_OFF")) { wr(SEG0(0x1688), 0x01fffe00); wr(SEG0(0x16ab + 17), 0x00F80001); usleep(2000); }   // MMHUB-like: no context0 translation
         uint64_t tgt = phys + 0x80000ull + 0x2000;                                     // inside the 1 MiB buffer
         uint32_t b = 8;
         vw(CMP_RING + b * 4 + 0, 0xC0033700); vw(CMP_RING + b * 4 + 4, 0x00100500);
         vw(CMP_RING + b * 4 + 8, (uint32_t)tgt); vw(CMP_RING + b * 4 + 12, (uint32_t)(tgt >> 32)); vw(CMP_RING + b * 4 + 16, 0xA6A6A6A6);
         b += 5;
         vw(CMP_RING + b * 4 + 0, 0xC0065800); vw(CMP_RING + b * 4 + 4, 0);                       // ACQUIRE_MEM: GL2 writeback+invalidate
         vw(CMP_RING + b * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + b * 4 + 12, 0x00FFFFFF);
         vw(CMP_RING + b * 4 + 16, 0); vw(CMP_RING + b * 4 + 20, 0);
         vw(CMP_RING + b * 4 + 24, 0x0A); vw(CMP_RING + b * 4 + 28, 0xC000);
         b += 8;
         vw(CMP_WB + 16, b); vw(CMP_WB + 20, 0); db(6 * 4, b); usleep(300000);
         // GPU READ tests with COPY_DATA (src_sel=2 mem via L2, dst_sel=5 mem), first VRAM->VRAM to validate the encoding, then system->VRAM
         {
            uint64_t pp0, vv0; sm(3, 0x80000ull + 0x3000, 0x5A5A5A5A, &pp0, &vv0);
            vw(TESTBUF + 0x100, 0x77777777); vw(TESTBUF + 0x110, 0); vw(TESTBUF + 0x114, 0);
            struct { uint64_t src, dst; } cp[2] = { { MC(TESTBUF + 0x100), MC(TESTBUF + 0x110) }, { tgt + 0x1000, MC(TESTBUF + 0x114) } };
            for (int k = 0; k < 2; k++) {
               vw(CMP_RING + b * 4 + 0, 0xC0044000); vw(CMP_RING + b * 4 + 4, 2 | (5u << 8) | (1u << 20));
               vw(CMP_RING + b * 4 + 8, (uint32_t)cp[k].src); vw(CMP_RING + b * 4 + 12, (uint32_t)(cp[k].src >> 32));
               vw(CMP_RING + b * 4 + 16, (uint32_t)cp[k].dst); vw(CMP_RING + b * 4 + 20, (uint32_t)(cp[k].dst >> 32));
               b += 6; vw(CMP_WB + 16, b); db(6 * 4, b); usleep(300000);
               printf("COPY_DATA test %d (%s): dst=0x%08x\n", k, k ? "system->VRAM" : "VRAM->VRAM", vr(TESTBUF + (k ? 0x114 : 0x110)));
            }
         }
         uint64_t pp, v = 0; sm(2, 0x80000ull + 0x2000, 0, &pp, &v);
         printf("AGP identity write to system memory phys 0x%llx: value=0x%08x (want a6a6a6a6)\n", (unsigned long long)tgt, (uint32_t)v);
         return;
      }
      const uint64_t vaoff = 0x7F00000;
      const int vram_t = getenv("MESBOOT_VMT")[0] == '2';
      const int pt_sys = getenv("PT_SYS") && *getenv("PT_SYS") == '1';   // page table itself in system memory
      uint64_t sysphys = phys;
      if (vram_t) phys = FB_PA + 0x2600000ull;
      else if (pt_sys) phys = sysphys + 0x80000ull;                      // targets after the table region
      uint32_t wp = 8, hit = 0;
      static const uint64_t lowf[] = {0x77, 0x71, 0x73, 0x75, 0x7f};
      for (int mt = 0; mt < 5; mt++) {
         for (int fl = 0; fl < 5; fl++) {
            int idx = mt * 5 + fl;
            uint64_t pte = (phys + idx * 4096ull) | lowf[fl] | ((uint64_t)mt << 48) | (getenv("PTE_EXTRA") && *getenv("PTE_EXTRA") ? strtoull(getenv("PTE_EXTRA"), 0, 0) : 0);
            uint64_t slot = (vaoff >> 12) + idx;
            if (pt_sys) { uint64_t d0, d1; sm(3, slot * 8, (uint32_t)pte, &d0, &d1); sm(3, slot * 8 + 4, (uint32_t)(pte >> 32), &d0, &d1); }
            else { vw(GART_PT + slot * 8, (uint32_t)pte); vw(GART_PT + slot * 8 + 4, (uint32_t)(pte >> 32)); }
         }
      }
      if (pt_sys) {
         wr(SEG0(0x16f3), (uint32_t)((sysphys & ~0xfffull) | (getenv("ROOT_FL") && *getenv("ROOT_FL") ? strtoul(getenv("ROOT_FL"), 0, 0) : 3))); wr(SEG0(0x16f4), (uint32_t)(sysphys >> 32));   // PDB base in SYSTEM memory
         printf("PT in system memory at phys 0x%llx\n", (unsigned long long)sysphys);
      }
      wr(SEG0(0x16ab + 17), 0x00F80001); usleep(2000);       // invalidate (eng17)
      const char *vo = getenv("VARONLY"); const int only = (vo && *vo) ? atoi(vo) : -1;
      for (int idx = 0; idx < 25; idx++) {
         if (only >= 0 && idx != only) continue;
         uint64_t gva = GART_VA + vaoff + idx * 4096ull;
         vw(CMP_RING + wp * 4 + 0, 0xC0033700); vw(CMP_RING + wp * 4 + 4, 0x00100500);
         vw(CMP_RING + wp * 4 + 8, (uint32_t)gva); vw(CMP_RING + wp * 4 + 12, (uint32_t)(gva >> 32)); vw(CMP_RING + wp * 4 + 16, 0xABCD0000 + idx);
         wp += 5;
      }
      if (!env_on("NO_GCR")) {   // write back + invalidate the GPU L2 so cached (non-UC) writes reach memory
         vw(CMP_RING + wp * 4 + 0, 0xC0065800); vw(CMP_RING + wp * 4 + 4, 0);
         vw(CMP_RING + wp * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + wp * 4 + 12, 0x00FFFFFF);
         vw(CMP_RING + wp * 4 + 16, 0); vw(CMP_RING + wp * 4 + 20, 0);
         vw(CMP_RING + wp * 4 + 24, 0x0A); vw(CMP_RING + wp * 4 + 28, 0xC000);
         wp += 8;
      }
      vw(CMP_WB + 16, wp); vw(CMP_WB + 20, 0);
      db(6 * 4, wp);
      usleep(300000);
      for (int idx = 0; idx < 25; idx++) {
         if (only >= 0 && idx != only) continue;
         uint64_t pp, val = 0; if (vram_t) val = vr(0x2600000 + idx * 4096ull); else sm(2, (pt_sys ? 0x80000ull : 0) + idx * 4096ull, 0, &pp, &val);
         if ((uint32_t)val == 0xABCD0000u + idx) { printf("  HIT: mtype=%d flags=0x%llx\n", idx / 5, (unsigned long long)lowf[idx % 5]); hit++; }
      }
      // read test: CPU puts a magic into system page 0, GPU COPY_DATA gart-VA -> MC testbuf
      if (env_on("COPYTEST")) {
         uint64_t pp, vv; if (vram_t) vw(0x2600000, 0xC0FFEE11); else sm(3, pt_sys ? 0x80000ull : 0, 0xC0FFEE11, &pp, &vv);
         vw(TESTBUF + 8, 0);
         uint64_t src = GART_VA + vaoff, dst = MC(TESTBUF + 8);
         uint32_t b = wp;
         vw(CMP_RING + b * 4 + 0, 0xC0044000); vw(CMP_RING + b * 4 + 4, 2 | (2u << 8) | (1u << 20));
         vw(CMP_RING + b * 4 + 8, (uint32_t)src); vw(CMP_RING + b * 4 + 12, (uint32_t)(src >> 32));
         vw(CMP_RING + b * 4 + 16, (uint32_t)dst); vw(CMP_RING + b * 4 + 20, (uint32_t)(dst >> 32));
         wp += 6; vw(CMP_WB + 16, wp); db(6 * 4, wp); usleep(200000);
         printf("GART read test: testbuf+8 = 0x%08x (want c0ffee11)\n", vr(TESTBUF + 8));
      }
      grbm_select(1, 0, 0, 0);
      printf("compute rptr=%u wptr_written=%u fault status=0x%x addr=0x%x:%x\n", rd(SEG0(0x1fb3)), wp, rd(SEG0(0x15c8)), rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
      grbm_select(0, 0, 0, 0);
      if (!vram_t) {   // where did the GPU writes go? scan the first dword of every page of the 1 MiB buffer
         int nz = 0;
         for (int pg = 0; pg < 256; pg++) { uint64_t pp, v = 0; sm(2, pg * 4096ull, 0, &pp, &v);
            if (pg >= 0x80 || !pt_sys) { if ((uint32_t)v) { printf("  page %d: 0x%08x\n", pg, (uint32_t)v); nz++; } } }
         printf("scan: %d non-zero target pages\n", nz);
      }
      printf("system-memory write via GART: %u hits of 25 variants (value=0x%08x)\n", hit, hit ? 0xabcd1234 : 0);
   }
}

// VMID 8 user context: 4-level page table in system memory, IB + target in VRAM pages, IB executed from the kernel compute ring
static void vm8_test(void)
{
   uint64_t phys = 0, len = 0, d0, d1;
   if (sm(1, 1 << 20, 0, &phys, &len)) { printf("vm8: sysmem alloc failed\n"); return; }
   const uint64_t T2 = phys, T1 = phys + 0x1000, T0 = phys + 0x2000, TP = phys + 0x3000;
   const uint64_t VA_IB = env_on("VM8_HI") ? 0x800100000000ull : 0x100000000ull, VA_T = VA_IB + 0x1000;   // VM8_HI: upper half (bit 47), like RADV
   const uint64_t IBO = 0x2700000ull, TGO = 0x2701000ull;           // VRAM offsets backing the two pages
   const unsigned mt = getenv("VM8_MT") ? (unsigned)atoi(getenv("VM8_MT")) : 4;
   const uint64_t flags = 0x71 | ((uint64_t)mt << 48);
   #define PUT(off, val) do { uint64_t v_ = (val); sm(3, (off), (uint32_t)v_, &d0, &d1); sm(3, (off) + 4, (uint32_t)(v_ >> 32), &d0, &d1); } while (0)
   const int V = 8;
   const int flat = env_on("VM8_FLAT");
   if (flat) {   // depth 0: one flat PTB covering 512 pages from VA_IB, like the working GART
      PUT(0x0000 + 0 * 8, (FB_PA + IBO) | flags);
      PUT(0x0000 + 1 * 8, (FB_PA + TGO) | flags);
      wr(SEG0(0x16f3 + 2 * V), (uint32_t)(T2 | 3)); wr(SEG0(0x16f4 + 2 * V), (uint32_t)(T2 >> 32));
      wr(SEG0(0x1688 + V), 0x01fffe01);
      wr(SEG0(0x1713 + 2 * V), (uint32_t)(VA_IB >> 12)); wr(SEG0(0x1714 + 2 * V), (uint32_t)(VA_IB >> 44));
      wr(SEG0(0x1733 + 2 * V), (uint32_t)((VA_IB >> 12) + 511)); wr(SEG0(0x1734 + 2 * V), (uint32_t)(VA_IB >> 44));
   } else {
      PUT(0x0000 + ((VA_IB >> 39) & 0x1ff) * 8, T1 | 7);
      PUT(0x1000 + ((VA_IB >> 30) & 0x1ff) * 8, T0 | 7);                     // gmc_v11: translate_further=false -> plain PDE
      PUT(0x2000 + ((VA_IB >> 21) & 0x1ff) * 8, TP | 7);
      if (env_on("VM8_IBSYS")) PUT(0x3000 + ((VA_IB >> 12) & 0x1ff) * 8, (phys + 0x20000) | 0x77 | ((uint64_t)mt << 48));   // IB page in SYSTEM memory
      else PUT(0x3000 + ((VA_IB >> 12) & 0x1ff) * 8, (FB_PA + IBO) | flags);
      if (env_on("VM8_SYS")) PUT(0x3000 + ((VA_T >> 12) & 0x1ff) * 8, (phys + 0x10000) | 0x77 | ((uint64_t)mt << 48));   // target = SYSTEM page
      else PUT(0x3000 + ((VA_T >> 12) & 0x1ff) * 8, (FB_PA + TGO) | flags);
      wr(SEG0(0x16f3 + 2 * V), (uint32_t)(T2 | 3)); wr(SEG0(0x16f4 + 2 * V), (uint32_t)(T2 >> 32));
      wr(SEG0(0x1688 + V), 0x01fffe07);
      wr(SEG0(0x1713 + 2 * V), 0); wr(SEG0(0x1714 + 2 * V), 0);
      wr(SEG0(0x1733 + 2 * V), 0xffffffff); wr(SEG0(0x1734 + 2 * V), 0xf);
   }
   #undef PUT
   wr(SEG0(0x16ab + 17), 0x00F80000 | (1u << V) | 1u); usleep(2000);
   // IB: WRITE_DATA to VA_T
   const int ib0 = env_on("IB0");                       // control: same IB packet but VMID 0 + MC addresses (no VM involved)
   // canonical (sign-extended) form for upper-half VAs, as amdgpu passes them: bit 47 replicated into 63:48
   #define SEXT(x) (((x) & (1ull << 47)) ? ((x) | 0xFFFF000000000000ull) : (x))
   const uint64_t ibva = ib0 ? MC(IBO) : SEXT(VA_IB), tva = ib0 ? MC(TGO) : SEXT(VA_T); const int vmid = ib0 ? 0 : V;
   vw(IBO + 0, 0xC0033700); vw(IBO + 4, 0x00100500); vw(IBO + 8, (uint32_t)tva); vw(IBO + 12, (uint32_t)(tva >> 32)); vw(IBO + 16, 0x1BADB002);
   if (env_on("VM8_IBSYS")) {   // same IB, but in the system page the PTE points at
      const uint32_t ibw[5] = {0xC0033700, 0x00100500, (uint32_t)tva, (uint32_t)(tva >> 32), 0x1BADB002};
      for (int i = 0; i < 5; i++) sm(3, 0x20000 + i * 4, ibw[i], &d0, &d1);
   }
   vw(TGO, 0);
   uint32_t b = 8;
   vw(CMP_RING + b * 4 + 0, 0xC0023F00); vw(CMP_RING + b * 4 + 4, (uint32_t)ibva); vw(CMP_RING + b * 4 + 8, (uint32_t)(ibva >> 32));
   vw(CMP_RING + b * 4 + 12, 5 | (1u << 23) | ((uint32_t)vmid << 24));   // IB_SIZE | VALID | VMID
   b += 4;
   // GL2 writeback+invalidate so the IB's write leaves the GPU cache (same ACQUIRE_MEM as the working GART test)
   vw(CMP_RING + b * 4 + 0, 0xC0065800); vw(CMP_RING + b * 4 + 4, 0);
   vw(CMP_RING + b * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + b * 4 + 12, 0x00FFFFFF);
   vw(CMP_RING + b * 4 + 16, 0); vw(CMP_RING + b * 4 + 20, 0);
   vw(CMP_RING + b * 4 + 24, 0x0A); vw(CMP_RING + b * 4 + 28, 0xC000);
   b += 8;
   // RELEASE_MEM fence exactly as the kext emits it -> MC 0x2550000
   vw(0x2550000, 0); vw(0x2550004, 0);
   { const uint64_t fm = MC(0x2550000ull); const uint32_t rm[8] = {0xC0064900, 0x06603514, 0x40000000, (uint32_t)fm, (uint32_t)(fm >> 32), 0x77, 0, 0};
     for (int i = 0; i < 8; i++) vw(CMP_RING + (b + i) * 4, rm[i]); b += 8; }
   vw(CMP_WB + 16, b); vw(CMP_WB + 20, 0); db(6 * 4, b);
   const int tsys = env_on("VM8_SYS");
   uint64_t sv = 0;
   for (int i = 0; i < 50; i++) { usleep(10000); if (tsys) { uint64_t pp; sm(2, 0x10000, 0, &pp, &sv); if ((uint32_t)sv == 0x1BADB002) break; } else if (vr(TGO) == 0x1BADB002) break; }
   if (tsys) printf("vm8 SYSTEM target: 0x%08x (want 1badb002)\n", (uint32_t)sv);
   grbm_select(1, 0, 0, 0);
   printf("vm8 RELEASE_MEM fence = 0x%08x (want 77)\n", vr(0x2550000));
   printf("vm8 test (mtype %u): target=0x%08x (want 1badb002) rptr=%u fault=0x%x addr=0x%x:%x\n", mt, vr(TGO), rd(SEG0(0x1fb3)), rd(SEG0(0x15c8)), rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
   grbm_select(0, 0, 0, 0);
}

static void sched_test(void)
{
   sched_init();
   union MESAPI_SET_HW_RESOURCES r;
   memset(&r, 0, sizeof(r));
   r.header.type = MES_API_TYPE_SCHEDULER;
   r.header.opcode = MES_SCH_API_SET_HW_RSRC;
   r.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   // As Linux amdgpu_mes_init on this hardware ("MES: vmid_mask_mmhub 0x0000ff00, vmid_mask_gfxhub 0x0000ff00,
   // gfx_hqd_mask 0x00000002, compute_hqd_mask 0x0000000c, sdma_hqd_mask 0x000000fc"): 16 VMIDs exist, and gfx pipe 0
   // has two queues, of which queue 0 is the kernel (legacy) queue mapped below.
   r.vmid_mask_mmhub = 0x0000ff00;
   r.vmid_mask_gfxhub = 0x0000ff00;
   r.gds_size = 0;
   r.paging_vmid = 0;
   r.g_sch_ctx_gpu_mc_ptr = VA(SCH_WB + 128);
   r.query_status_fence_gpu_mc_ptr = VA(SCH_WB + 136);
   for (int i = 0; i < 4; i++) r.compute_hqd_mask[i] = 0xc;
   r.gfx_hqd_mask[0] = 0x2;
   r.sdma_hqd_mask[0] = 0xfc;
   // adev->reg_offset[GC/MMHUB/OSSSYS][0][0..4], from the chip profile (Strix's base 3 differs from Phoenix's)
   if (!chip->mes.gc[0] || !chip->mes.mmhub[0] || !chip->mes.osssys[0]) {
      fprintf(stderr, "mesboot: no MES IP bases for %s in rdna3_chip.h (tools/check_strix_discovery.py prints them); "
                      "not starting the scheduler\n", chip->name);
      return;
   }
   for (int i = 0; i < 5; i++) {
      r.gc_base[i] = chip->mes.gc[i]; r.mmhub_base[i] = chip->mes.mmhub[i]; r.osssys_base[i] = chip->mes.osssys[i];
   }
   r.disable_reset = 1; r.disable_mes_log = 1; r.use_different_vmid_compute = 1;
   r.enable_reg_active_poll = 1; r.enable_level_process_quantum_check = 1;
   // oversubscription_timer stays 0: Linux arms it only under SR-IOV (bare metal: KFD's software replacement)
   mes_submit(&r, sizeof(r), offsetof(union MESAPI_SET_HW_RESOURCES, api_status), "SET_HW_RESOURCES");
   // Linux mes_v11_0_hw_init, MES >= 0x52 (the 880M's is 0x92): SET_HW_RESOURCES_1 with the cleaner shader fence, then,
   // with the cleaner shader enabled (GC 11.5.0: MEC >= 26, MES >= 114), CHANGE_CONFIG limit_single_process = 0.
   // Without them the firmware keeps a garbage fence address: try7..try9's VMID 0 CPG read of an unmapped page at the
   // first gfx IB and at every new VMID context, constant within a boot (CP_GFX_ERROR.SUA_ERROR). Strix only by default
   // (Phoenix runs without it); MESBOOT_RSRC1=0/1 overrides.
   const int rsrc1 = getenv("MESBOOT_RSRC1") ? env_on("MESBOOT_RSRC1") : chip->id == RDNA3_CHIP_STRIX_POINT;
   if (rsrc1) {
      for (int i = 0; i < 1024; i++) vw(MES_RES1 + i * 4, 0);
      union MESAPI_SET_HW_RESOURCES_1 r1;
      memset(&r1, 0, sizeof(r1));
      r1.header.type = MES_API_TYPE_SCHEDULER;
      r1.header.opcode = MES_SCH_API_SET_HW_RSRC_1;
      r1.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
      r1.enable_mes_info_ctx = 1;
      r1.cleaner_shader_fence_mc_addr = VA(MES_RES1);
      mes_submit(&r1, sizeof(r1), offsetof(union MESAPI_SET_HW_RESOURCES_1, api_status), "SET_HW_RESOURCES_1");
   }
   union MESAPI__QUERY_MES_STATUS q;
   memset(&q, 0, sizeof(q));
   q.header.type = MES_API_TYPE_SCHEDULER;
   q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
   q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   mes_submit(&q, sizeof(q), offsetof(union MESAPI__QUERY_MES_STATUS, api_status), "QUERY_SCHED_STATUS");
   if (rsrc1) {   // amdgpu_mes_update_enforce_isolation with enforce_isolation off
      union MESAPI__MISC m;
      memset(&m, 0, sizeof(m));
      m.header.type = MES_API_TYPE_SCHEDULER;
      m.header.opcode = MES_SCH_API_MISC;
      m.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
      m.opcode = MESAPI_MISC__CHANGE_CONFIG;
      m.change_config.opcode = MESAPI_MISC__CHANGE_CONFIG_OPTION_LIMIT_SINGLE_PROCESS;
      m.change_config.option.bits.limit_single_process = 0;
      mes_submit(&m, sizeof(m), offsetof(union MESAPI__MISC, api_status), "MISC CHANGE_CONFIG(limit_single_process=0)");
   }
   if (env_on("MESBOOT_CS")) compute_via_mes();
   if (env_on("MESBOOT_GFX")) gfx_via_mes();
}

#define MEC_RING 0x2530000ull
#define MEC_WB   0x2531000ull
#define MEC_MQD  0x2532000ull
#define MEC_EOP  0x2533000ull
static void mec_test(void)
{
   printf("MEC: CP_MEC_CNTL=0x%x before\n", rd(SEG1(0x802)));
   wr(SEG1(0x802), 0);                                  // unhalt ME1 + ME2
   usleep(5000);
   printf("MEC: CP_MEC_CNTL=0x%x after, RS64_CNTL=0x%x\n", rd(SEG1(0x802)), rd(SEG1(0x2904)));
   for (int i = 0; i < 1024; i++) vw(MEC_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(MEC_WB + i * 4, 0);
   mqd_build(MEC_MQD, MEC_RING, MEC_WB, MEC_EOP, 1, 6);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   hqd_program(1, 0, MEC_MQD, MEC_RING, MEC_WB, pq, (6u << 2) | (1u << 30));
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(MEC_RING + 0, 0xC0017900); vw(MEC_RING + 4, 0x40); vw(MEC_RING + 8, 0xDEADBEEF);
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   vw(MEC_RING + 12, 0xC0033700); vw(MEC_RING + 16, 0x00100500);
   vw(MEC_RING + 20, (uint32_t)VA(TESTBUF)); vw(MEC_RING + 24, (uint32_t)(VA(TESTBUF) >> 32)); vw(MEC_RING + 28, 0xFEEDF00D);
   db(6 * 4, 8);
   usleep(100000);
   grbm_select(1, 0, 0, 0);
   printf("MEC hqd: active=%u rptr=%u scratch0=0x%08x (expect deadbeef)\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb3)), rd(SEG1(0x2040)));
   printf("MEC testbuf=0x%08x (expect feedf00d)\n", vr(TESTBUF));
   grbm_select(0, 0, 0, 0);
}

// GFX (graphics) kernel queue: me0 pipe0 queue0 mapped by MES ADD_QUEUE(type GFX, map_legacy_kq), MQD per gfx_v11_0_gfx_mqd_init.
// Doorbell 0x116 (dword) inside the RB doorbell range Linux programs (LOWER 0x458 / UPPER 0x7f8, byte units).
#define GFX_RING 0x2570000ull            // 8 KiB ring (2048 dw: the clear-state preamble alone is 960)
#define GFX_RDW  2048u
#define GFX_WB   0x2574000ull            // +0 rptr report, +16 wptr poll
#define GFX_MQD  0x2575000ull            // v11_gfx_mqd, 2 KiB
#define GFX_CSB  0x2576000ull            // RLC clear-state buffer (RLC_CSIB_*), as gfx_v11_0_init_csb
#define GFX_DB   0x116u
#include "csb_gfx11.h"
#include "cp_regs_gc11_5.h"
// GCVM_L2_PROTECTION_FAULT_CNTL.CLEAR_PROTECTION_FAULT_STATUS_ADDR (bit 0), as Linux's gmc code does it
static void fault_clear(void)
{
   const uint32_t c = rd(SEG0(0x15c4));
   wr(SEG0(0x15c4), c | 1); usleep(100); wr(SEG0(0x15c4), c & ~1u);
}
static uint32_t rd_abs(uint32_t reg)   // no GCVM translation: for registers already given in GC 11.5 terms
{
   uint64_t in[3] = {reg, 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
   IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
   return v;
}
static void wr_abs(uint32_t reg, uint32_t val)
{
   uint64_t in[2] = {reg, val}, out[1] = {0}; uint32_t n = 1;
   IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
}
// One WRITE_DATA IB on the gfx ring in VMID v (1..7), whose context is pointed at the same flat GART table as VMID 0:
// the IB and its target are reached through the GART VA. The first IB of a VMID context is what faults (try9..try12),
// so every call with a fresh v is one more trial of the "new context" fault in the same boot.
static void gfx_vmid_test(uint32_t *wp, uint32_t v, const char *what)
{
   uint32_t w = *wp;
   const uint64_t ibo = TESTBUF + 0x600 + (uint64_t)v * 0x40, to = TESTBUF + 0x500 + (uint64_t)v * 4, fen = MC(0x2580000);
   const uint64_t ibva = GART_VA + ibo, tva = GART_VA + to;
   const uint32_t want = 0x5EED0000u + v, fv = 0x100u + v;
   wr(SEG0(0x16f3 + 2 * v), (uint32_t)((FB_PA + GART_PT) | 1)); wr(SEG0(0x16f4 + 2 * v), (uint32_t)((FB_PA + GART_PT) >> 32));
   wr(SEG0(0x1713 + 2 * v), (uint32_t)(GART_VA >> 12)); wr(SEG0(0x1714 + 2 * v), (uint32_t)(GART_VA >> 44));
   wr(SEG0(0x1733 + 2 * v), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 12));
   wr(SEG0(0x1734 + 2 * v), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 44));
   wr(SEG0(0x1688 + v), 0x01fffe01);                                     // enabled, flat (depth 0) like context 0
   wr(SEG0(0x16ab + 17), 0x00F80000 | (1u << v)); usleep(2000);          // invalidate that VMID on engine 17
   vw(to, 0); vw(0x2580000, 0); vw(0x2580004, 0);
   const uint32_t ibw[8] = {0xC0033700, 0x00100500, (uint32_t)tva, (uint32_t)(tva >> 32), want, 0xFFFF1000, 0xFFFF1000, 0xFFFF1000};
   for (int i = 0; i < 8; i++) vw(ibo + i * 4, ibw[i]);
   fault_clear();
   const uint32_t rw[12] = {0xC0023F00, (uint32_t)ibva, (uint32_t)(ibva >> 32), 8 | (v << 24),
                            0xC0064900, 0x06603514, 0x40000000, (uint32_t)fen, (uint32_t)(fen >> 32), fv, 0, 0};
   for (int i = 0; i < 12; i++) vw(GFX_RING + (w + i) * 4, rw[i]);
   w += 12;
   vw(GFX_WB + 16, w); vw(GFX_WB + 20, 0);
   db(GFX_DB * 4, w);
   for (int i = 0; i < 50; i++) { usleep(10000); if (vr(0x2580000) == fv) break; }
   const uint32_t st = rd(SEG0(0x15c8));
   printf("vmid %u IB after %-40s: target=0x%08x (want %08x) fence=0x%x  -> %s (status 0x%x addr 0x%x:%x)\n", v, what,
          vr(to), want, vr(0x2580000), st ? "FAULT" : "no fault", st, rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
   *wp = w;
}
// What Linux writes in GC before its first gfx IB and mesboot did not (tools/linux-trace on the 880M, amdgpu 7.2.9)
static void linux_delta(int k)
{
   switch (k) {
   case 1: wr_abs(0xec80, rd_abs(0xec80) | 3); break;                    // RLC_SRM_CNTL: SRM_ENABLE | AUTO_INCR_ADDR
   case 2: wr_abs(0xa982, 0xf); break;                                    // RLC_SPM_MC_CNTL: RLC_SPM_VMID 0xf
   case 3:                                                               // SPI_GDBG_PER_VMID_CNTL = 8 (TRAP_EN), VMIDs 8..15
      for (uint32_t v = 8; v < 16; v++) { wr(SEG1(0x900), v << 4); wr_abs(0x31d2, 8); }
      wr(SEG1(0x900), 0); break;
   case 4:                                                               // GDS / GWS / OA of VMIDs 1..15 = 0
      for (uint32_t v = 1; v < 16; v++) { wr_abs(0x3300 + 2 * v, 0); wr_abs(0x3301 + 2 * v, 0); wr_abs(0x3320 + v, 0); wr_abs(0x3330 + v, 0); }
      break;
   case 5: wr_abs(0x2000, 0xff); wr_abs(0xb9a2, rd_abs(0xb9a2) | 0x20000000); wr_abs(0xc200, 0xe0000000); break;   // GRBM_CNTL, TCP_CNTL golden, GRBM_GFX_INDEX broadcast
   }
}
static const char *kDelta[6] = {"nothing (control: does a new VMID fault?)", "RLC_SRM_CNTL |= 3 (SRM on)",
   "RLC_SPM_MC_CNTL = 0xf", "SPI_GDBG_PER_VMID_CNTL = 8 (VMID 8-15)", "GDS/GWS/OA VMID 1-15 = 0",
   "GRBM_CNTL 0xff, TCP_CNTL golden, GFX_INDEX"};
// save != NULL: print the non-zero registers and keep them; prev != NULL: print what changed since, then look for the
// latched GCVM fault address in one register or a lo/hi pair (as is, <<8 for 256-byte bases, <<12 for page numbers)
static void cpscan(const char *tag, uint32_t *save, const uint32_t *prev)
{
   if (chip->id != RDNA3_CHIP_STRIX_POINT) { printf("cpscan: the table is GC 11.5 only\n"); return; }
   const unsigned n = sizeof(kCpRegsGc115) / sizeof(kCpRegsGc115[0]);
   static uint32_t v[sizeof(kCpRegsGc115) / sizeof(kCpRegsGc115[0])];
   unsigned shown = 0;
   for (unsigned i = 0; i < n; i++) {
      v[i] = rd_abs(kCpRegsGc115[i].reg);
      if (save) save[i] = v[i];
      if (prev ? v[i] != prev[i] : v[i] != 0) {
         if (prev) printf("cpscan %s: %s (0x%05x) 0x%08x -> 0x%08x\n", tag, kCpRegsGc115[i].name, kCpRegsGc115[i].reg, prev[i], v[i]);
         else printf("cpscan %s: %s (0x%05x) = 0x%08x\n", tag, kCpRegsGc115[i].name, kCpRegsGc115[i].reg, v[i]);
         shown++;
      }
   }
   printf("cpscan %s: %u of %u registers %s\n", tag, shown, n, prev ? "changed" : "non-zero");
   if (!prev) return;
   const uint32_t st = rd(SEG0(0x15c8)), hi = rd(SEG0(0x15ca)), lo = rd(SEG0(0x15c9));
   if (!st) { printf("cpscan: no GCVM fault latched\n"); return; }
   const uint64_t page = ((uint64_t)(hi & 0xf) << 32) | lo, va = page << 12;
   unsigned hits = 0;
   for (unsigned i = 0; i < n; i++) {
      const uint64_t one = v[i];
      const uint64_t pair = (i + 1 < n && kCpRegsGc115[i + 1].reg == kCpRegsGc115[i].reg + 1) ? ((uint64_t)v[i + 1] << 32) | v[i] : one;
      const uint64_t c[6] = {one << 12, one << 8, pair, pair << 8, pair << 12, one};
      static const char *how[6] = {"page", "256 B base", "lo/hi pair", "lo/hi pair << 8", "lo/hi pair << 12", "low 32 bits"};
      for (int k = 0; k < 6; k++) {
         const uint64_t x = c[k] & 0xFFFFFFFFF000ull;
         if (k == 5 ? ((uint32_t)va >> 12) && ((uint32_t)x >> 12) == ((uint32_t)va >> 12) : x == va) {
            printf("cpscan: fault address 0x%llx matches %s (0x%05x) as %s\n", (unsigned long long)va, kCpRegsGc115[i].name,
                   kCpRegsGc115[i].reg, how[k]);
            hits++;
         }
      }
   }
   if (!hits) printf("cpscan: fault address 0x%llx is in no CP register\n", (unsigned long long)va);
}
// One WRITE_DATA IB on the gfx ring, VMID 0, MC addresses, then a RELEASE_MEM fence. lnx=1 wraps the IB as amdgpu does
// for every gfx job: CONTEXT_CONTROL (load_enable), FRAME_CONTROL start, IB, FRAME_CONTROL end. Test 0 keeps try7's
// target/value/fence and output line so the logs compare.
static void gfx_ib_test(uint32_t *wp, int k, int lnx)
{
   uint32_t w = *wp;
   const uint64_t ibo = TESTBUF + 0x200 + (uint64_t)k * 0x40, to = k ? TESTBUF + 0x60 + (uint64_t)k * 4 : TESTBUF + 0x44;
   const uint64_t ib = MC(ibo), tgt = MC(to), fen = MC(0x2580000);
   const uint32_t want = 0x1BADB0B0u + (uint32_t)k, fv = 0x77u + (uint32_t)k;
   vw(to, 0); vw(0x2580000, 0); vw(0x2580004, 0);
   const uint32_t ibw[8] = {0xC0033700, 0x00100500, (uint32_t)tgt, (uint32_t)(tgt >> 32), want,
                            0xFFFF1000, 0xFFFF1000, 0xFFFF1000};          // padded with gfx11's NOP, as amdgpu does
   for (int i = 0; i < 8; i++) vw(ibo + i * 4, ibw[i]);
   if (k) fault_clear();
   const uint32_t before = rd(SEG0(0x15c8)), gerr0 = rd(SEG0(0x1ddb));
   uint32_t rw[24], n = 0;
   if (lnx) { rw[n++] = 0xC0012800; rw[n++] = 0x80000000; rw[n++] = 0; rw[n++] = 0xC0009000; rw[n++] = 0; }
   // INDIRECT_BUFFER (gfx form: size | VMID, no VALID bit)
   rw[n++] = 0xC0023F00; rw[n++] = (uint32_t)ib; rw[n++] = (uint32_t)(ib >> 32); rw[n++] = 8;
   if (lnx) { rw[n++] = 0xC0009000; rw[n++] = 0x10000000; }
   const uint32_t rm[8] = {0xC0064900, 0x06603514, 0x40000000, (uint32_t)fen, (uint32_t)(fen >> 32), fv, 0, 0};
   for (int i = 0; i < 8; i++) rw[n++] = rm[i];
   for (uint32_t i = 0; i < n; i++) vw(GFX_RING + (w + i) * 4, rw[i]);
   w += n;
   vw(GFX_WB + 16, w); vw(GFX_WB + 20, 0);
   db(GFX_DB * 4, w);
   for (int i = 0; i < 50; i++) { usleep(10000); if (vr(0x2580000) == fv) break; }
   if (!k)
      printf("gfx IB vmid0: target=0x%08x (want 1badb0b0) fence=0x%x (want 77) rptr=%u fault before=0x%x after=0x%x addr=0x%x:%x\n",
             vr(to), vr(0x2580000), rd(SEG0(0x1e88)), before, rd(SEG0(0x15c8)), rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
   else
      printf("gfx IB #%d (%s): target=0x%08x (want %08x) fence=0x%x (want %x) fault=0x%x addr=0x%x:%x CP_GFX_ERROR 0x%x -> 0x%x\n",
             k, lnx ? "linux frame" : "plain", vr(to), want, vr(0x2580000), fv, rd(SEG0(0x15c8)), rd(SEG0(0x15ca)),
             rd(SEG0(0x15c9)), gerr0, rd(SEG0(0x1ddb)));
   *wp = w;
}
static void gfx_via_mes(void)
{
   for (unsigned i = 0; i < GFX_RDW; i++) vw(GFX_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(GFX_WB + i * 4, 0);
   for (int i = 0; i < 512; i++) vw(GFX_MQD + i * 4, 0);
   const unsigned ncsb = sizeof(kCsbGfx11) / 4;
   for (unsigned i = 0; i < ncsb; i++) vw(GFX_CSB + i * 4, kCsbGfx11[i]);
   wr(SEG1(0x988), (uint32_t)(MC(GFX_CSB) >> 32)); wr(SEG1(0x987), (uint32_t)MC(GFX_CSB) & 0xfffffffc); wr(SEG1(0x989), ncsb);   // RLC_CSIB_ADDR_HI/LO/LENGTH
   vw(TESTBUF + 0x40, 0);
   wr(SEG0(0x1dfa), GFX_DB << 2); wr(SEG0(0x1dfb), 0x7f8);           // CP_RB_DOORBELL_RANGE_LOWER/UPPER
   const uint64_t ring = MC(GFX_RING), rb = ring >> 8, rptr = MC(GFX_WB), wpoll = MC(GFX_WB + 16), mqd = MC(GFX_MQD);
   struct { uint32_t off, val; } m[] = {
      {128, (uint32_t)mqd & 0xfffffffc}, {129, (uint32_t)(mqd >> 32)},   // cp_mqd_base_addr
      {162, 0x00000100},                                                // cp_gfx_mqd_control: PRIV_STATE 1, VMID 0
      {131, 0}, {134, 0}, {135, 0x00000a01},                            // hqd_vmid, queue_priority, quantum (QUANTUM_EN)
      {136, (uint32_t)rb}, {137, (uint32_t)(rb >> 32)},                 // cp_gfx_hqd_base = ring >> 8
      {139, (uint32_t)rptr & 0xfffffffc}, {140, (uint32_t)(rptr >> 32) & 0xffff},
      {141, (uint32_t)wpoll & 0xfffffffc}, {142, (uint32_t)(wpoll >> 32) & 0xffff},
      // hqd_cntl: RB_BUFSZ 10 (2048 dw), RB_BLKSZ 8. No KMD_QUEUE (bit 29): Linux up to v7.2 never sets it for gfx and
      // the 880M's live CP_GFX_HQD_CNTL under Linux reads 0x00a0080a; with it the gfx CP made a VMID 0 read of a
      // garbage address at the first IB of every context (try7..try10). GFX_KMD=1 puts it back (master's MQD).
      {145, 0x00a00000 | 10u | (8u << 8) | (env_on("GFX_KMD") ? (1u << 29) : 0u)},
      {143, (GFX_DB << 2) | (1u << 30)},                                // cp_rb_doorbell_control: offset, DOORBELL_EN
      {138, 0}, {149, 0}, {150, 0},                                     // rptr, wptr, wptr_hi
      {130, 1},                                                         // cp_gfx_hqd_active
   };
   for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) vw(GFX_MQD + m[i].off * 4, m[i].val);
   union MESAPI__ADD_QUEUE a;
   memset(&a, 0, sizeof(a));
   a.header.type = MES_API_TYPE_SCHEDULER;
   a.header.opcode = MES_SCH_API_ADD_QUEUE;
   a.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   a.pipe_id = 0; a.queue_id = 0;
   a.doorbell_offset = GFX_DB;
   a.mqd_addr = mqd;
   a.wptr_addr = wpoll;
   a.queue_type = MES_QUEUE_TYPE_GFX;
   a.map_legacy_kq = 1;
   if (mes_submit(&a, sizeof(a), offsetof(union MESAPI__ADD_QUEUE, api_status), "ADD_QUEUE(gfx, map_legacy_kq)")) return;
   grbm_select(0, 0, 0, 0);
   printf("gfx hqd after map: active=%u base=0x%x cntl=0x%08x dbctl=0x%08x rptr=%u\n", rd(SEG0(0x1e80)), rd(SEG0(0x1e86)),
          rd(SEG0(0x1e8f)), rd(SEG0(0x1e8d)), rd(SEG0(0x1e88)));
   wr(SEG1(0x2041), 0xCAFEDEAD);
   uint32_t w = 0;
   if (!env_on("NO_CSB")) for (unsigned i = 0; i < ncsb; i++) vw(GFX_RING + w++ * 4, kCsbGfx11[i]);   // gfx_v11_0_cp_gfx_start
   vw(GFX_RING + w++ * 4, 0xC0017900); vw(GFX_RING + w++ * 4, 0x41); vw(GFX_RING + w++ * 4, 0xDEADBEEF);   // SET_UCONFIG_REG scratch1
   vw(GFX_RING + w++ * 4, 0xC0033700); vw(GFX_RING + w++ * 4, 0x00100500);                                   // WRITE_DATA mem, wr confirm
   vw(GFX_RING + w++ * 4, (uint32_t)MC(TESTBUF + 0x40)); vw(GFX_RING + w++ * 4, (uint32_t)(MC(TESTBUF + 0x40) >> 32));
   vw(GFX_RING + w++ * 4, 0x6F7A6F7A);
   vw(GFX_WB + 16, w); vw(GFX_WB + 20, 0);
   db(GFX_DB * 4, w);
   for (int i = 0; i < 50; i++) { usleep(10000); if (rd(SEG1(0x2041)) == 0xDEADBEEF && vr(TESTBUF + 0x40) == 0x6F7A6F7A) break; }
   grbm_select(0, 0, 0, 0);
   printf("gfx: scratch1=0x%08x (want deadbeef) testbuf=0x%08x (want 6f7a6f7a) rptr=%u rptr_wb=%u grbm=0x%08x cp_stat=0x%08x\n",
          rd(SEG1(0x2041)), vr(TESTBUF + 0x40), rd(SEG0(0x1e88)), vr(GFX_WB), rd(0x2004), rd(0x21a0));
   if (env_on("GFXIB0")) {
      // CPSCAN: every readable CP register before and after the first IB, and which of them holds the faulting address
      static uint32_t cp0[sizeof(kCpRegsGc115) / sizeof(kCpRegsGc115[0])];
      if (env_on("CPSCAN")) cpscan("before the first gfx IB", cp0, NULL);
      // the same WRITE_DATA, but from an IB (as every real submission is): VMID 0, MC addresses
      gfx_ib_test(&w, 0, 0);
      if (env_on("CPSCAN")) cpscan("after the first gfx IB", NULL, cp0);
      if (env_on("GFXIB_MATRIX")) {
         // Which IBs fault, and whether what Linux does around every gfx IB (CONTEXT_CONTROL with load_enable,
         // FRAME_CONTROL start/end) or at gfx start (CP_MAX_CONTEXT, CP_DEVICE_ID) changes it. Faults cleared each time.
         gfx_ib_test(&w, 1, 0);
         gfx_ib_test(&w, 2, 1);
         gfx_ib_test(&w, 3, 1);
         wr(SEG0(0x1e4e), 7);   // CP_MAX_CONTEXT = max_hw_contexts - 1 (gfx_v11_0_cp_gfx_start)
         wr(SEG0(0x1deb), 1);   // CP_DEVICE_ID
         printf("gfx: CP_MAX_CONTEXT=%u CP_DEVICE_ID=%u written as Linux does\n", rd(SEG0(0x1e4e)), rd(SEG0(0x1deb)));
         gfx_ib_test(&w, 4, 1);
         gfx_ib_test(&w, 5, 0);
      }
      if (env_on("VMID_MATRIX")) {
         // the first IB of each new VMID faults: apply Linux's writes one at a time, a fresh VMID after each
         for (int k = 0; k <= 5; k++) {
            linux_delta(k);
            gfx_vmid_test(&w, (uint32_t)(k + 1), kDelta[k]);
         }
         gfx_vmid_test(&w, 7, "all of them, one more fresh VMID");
      }
   }
   if (env_on("ADOPT_GFX")) {   // hand the gfx queue to the kext as its second queue
      uint64_t in[6] = {GFX_RING, GFX_RDW, GFX_WB, GFX_DB * 4, w, 0x2580000 | 1};
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTQ, in, 6, NULL, NULL);
      printf("adopt gfx queue -> kr 0x%x\n", kr);
   }
}

static void sched_test(void);
static void kiq_init_only(void)
{
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, 1, 0x18);
   for (int i = 0; i < 1024; i++) vw(KIQ_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(KIQ_WB + i * 4, 0);
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, 0xd8308011, (0x18u << 2) | (1u << 30));
   printf("kiq init: RLC_CP_SCHEDULERS=0x%x\n", rd(SEG1(0x98a)));
}
static void kiqreg_test(void)
{
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, 1, 0x18);
   for (int i = 0; i < 1024; i++) vw(KIQ_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(KIQ_WB + i * 4, 0);
   uint32_t pq = 0xd8308011;
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, pq, (0x18u << 2) | (1u << 30));
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(KIQ_RING + 0, 0xC0017900); vw(KIQ_RING + 4, 0x40); vw(KIQ_RING + 8, 0xDEADBEEF);
   vw(KIQ_WB + 16, 3); vw(KIQ_WB + 20, 0);
   db(0x18 * 4, 3);
   for (int i = 0; i < 20; i++) { usleep(10000); if (rd(SEG1(0x2040)) == 0xDEADBEEF) break; }
   grbm_select(3, 1, 0, 0);
   printf("kiqreg: scratch0=0x%08x (expect deadbeef) rptr=%u wb_rptr=0x%x\n", rd(SEG1(0x2040)), rd(SEG0(0x1fb3)), vr(KIQ_WB));
   grbm_select(0, 0, 0, 0);
}

// SDMA0 queue0: WRITE_LINEAR of one dword to an arbitrary 64-bit address (ring in VRAM at 0x2000000, MC addressed)
static void sdma_write(uint64_t dst, uint32_t val)
{
   const uint64_t ring = 0x2000000ull;
   for (int i = 0; i < 8; i++) vw(ring + i * 4, 0);
   vw(ring + 0, 0x00000002); vw(ring + 4, (uint32_t)dst); vw(ring + 8, (uint32_t)(dst >> 32)); vw(ring + 12, 0); vw(ring + 16, val);
   wr(0x12E0, 0x00040800);
   wr(0x12E3, 0); wr(0x12E4, 0); wr(0x12E5, 0); wr(0x12E6, 0);
   wr(0x1313, 0x02001010); wr(0x1312, 0x80); wr(0x12E8, 0x80); wr(0x12E9, 0x02001000);
   wr(0x12E0, 0x00841814); wr(0x12E1, 0x80020000); wr(0x12E2, 0);
   wr(0x1315, 1); wr(0x12E5, 0); wr(0x12E6, 0); wr(0x1315, 0);
   wr(0xF89A, 0x08084000);
   wr(0x12E0, 0x00841815); wr(0x12EA, 0x101);
   vw(0x2001010, 0x14);
   wr(0x1315, 1); wr(0x12E5, 0x14); wr(0x12E6, 0); wr(0x1315, 0);
   usleep(200000);
}
// Queue and CP state of the bring-up's gfx and compute queues (the ones the kext adopts), and where the CP stalls.
static void gpustat(void)
{
   grbm_select(0, 0, 0, 0);
   printf("gfx  hqd: active=%u rptr=%u wptr=%u | wb: rptr_report=%u wptr_poll=%u | fence=%u:%u\n",
          rd(SEG0(0x1e80)), rd(SEG0(0x1e88)), rd(SEG0(0x1e91)), vr(GFX_WB), vr(GFX_WB + 16), vr(0x2580004), vr(0x2580000));
   grbm_select(1, 0, 0, 0);
   const uint32_t ca = rd(SEG0(0x1fab)), cr = rd(SEG0(0x1fb3)), cw = rd(SEG0(0x323f));
   grbm_select(0, 0, 0, 0);
   printf("comp hqd: active=%u rptr=%u wptr=%u | wb: wptr_poll=%u | fence=%u:%u\n",
          ca, cr, cw, vr(CMP_WB + 16), vr(0x2550004), vr(0x2550000));
   printf("GRBM_STATUS=0x%08x GRBM_STATUS2=0x%08x CP_STAT=0x%08x CP_BUSY_STAT=0x%08x\n",
          rd(0x2004), rd(0x2002), rd(0x21a0), rd(0x219f));
   printf("CP_PFP_HEADER_DUMP=0x%08x CP_ME_HEADER_DUMP=0x%08x CP_ME_INSTR_PNTR=0x%08x CP_MEC1_INSTR_PNTR=0x%08x\n",
          rd(0x21a2), rd(0x21a1), rd(0x21a6), rd(0x21a8));
   const uint32_t st = rd(SEG0(0x15c8)), hi = rd(SEG0(0x15ca)), lo = rd(SEG0(0x15c9));
   printf("VM fault: status=0x%08x addr=0x%x:%08x (page %#llx)\n", st, hi, lo,
          (unsigned long long)((((uint64_t)hi << 32) | lo) << 12));
   // where the CP engines fetch their code (inside the PSP TMR when the bring-up did SETUP_TMR), and what the gfx
   // CP last hit: CP_GFX_ERROR (bit 1 = PFP instruction cache UTCL1 error), the current IB1
   printf("CP fetch: PFP_IC=%08x:%08x ME_IC=%08x:%08x MEC_IC=%08x:%08x CP_GFX_ERROR=0x%08x IB1=%08x:%08x/%u PFP_PC=0x%x\n",
          rd(SEG1(0x5841)), rd(SEG1(0x5840)), rd(SEG1(0x5845)), rd(SEG1(0x5844)), rd(SEG1(0x584d)), rd(SEG1(0x584c)),
          rd(SEG0(0x1ddb)), rd(SEG1(0x20cd)), rd(SEG1(0x20cc)), rd(SEG1(0x20ce)), rd(SEG0(0x0f45)));
}

static void sdma_sys_test(void)
{
   uint64_t phys = 0, len = 0, v = 0;
   if (sm(1, 1 << 20, 0, &phys, &len)) { printf("sysmem alloc failed\n"); return; }
   // MMHUB AGP aperture identity (BASE=0, BOT=0, TOP=0x1ff): MC address == system physical address for SDMA
   wr(0x1a8f0, 0); wr(0x1a8ef, 0); wr(0x1a8ee, 0x000001ff);
   uint64_t tgt = phys + 0x1000;
   sdma_write(tgt, 0x5D5D5D5D);
   uint64_t pp; sm(2, 0x1000, 0, &pp, &v);
   printf("SDMA write to system phys 0x%llx via MMHUB AGP: 0x%08x (want 5d5d5d5d)\n", (unsigned long long)tgt, (uint32_t)v);
   sdma_write(MC(TESTBUF + 0x200), 0x6E6E6E6E);
   printf("SDMA control write to VRAM: 0x%08x (want 6e6e6e6e)\n", vr(TESTBUF + 0x200));
}

// dcnflip [seconds]: can the display engine scan a frame out of system RAM (the step before zero-copy scan-out)?
// A physically contiguous 1920x1200 buffer in system RAM gets five colour bands at its top (red, green, blue, yellow,
// magenta, 48 rows each; every word written through the kext with a clflush, so DRAM holds them). Pipe 0 is then set
// up the way Linux's DC sets a pipe for memory outside the carve-out (dcn31 hubbub init_dchub_sys_ctx + hubp3
// set_vm_system_aperture_settings): the DCN AGP aperture just above the FB (MC 0x81_0000_0000 + P = system physical
// P), the pipe's system aperture covering FB and AGP, and its L1 TLB on with SYSTEM_ACCESS_MODE 3. The first try left
// the pipe's aperture at 0 and its TLB off (the firmware's state): the AGP address went out untranslated and the panel
// read all ones, white (2026-10-26). After 50 ms on the old frame with the new settings (an underflow there stops
// it), the pipe flips to the buffer for <seconds>, flips back, and every register is put back - also on Ctrl-C.
#define DCN_AGP_BOT 0x3938u
#define DCN_AGP_TOP 0x3939u
#define DCN_AGP_BASE 0x393au
#define HUBP0_APT_LOW 0x3af4u
#define HUBP0_APT_HIGH 0x3af5u
#define HUBP0_TLB_CNTL 0x3b02u
#define HUBP0_ADDR_HI 0x3acbu
#define HUBP0_ADDR_LO 0x3acau
#define HUBP0_INUSE_HI 0x3ae5u
#define HUBP0_INUSE_LO 0x3ae4u
#define HUBP0_DCHUBP_CNTL 0x3ab3u
#define HUBP0_PITCH 0x3ac7u
#define HUBP0_VIEWPORT 0x3aaau
#define DCHVM_CTRL0 0x6ac3u
#define DCN_LOCAL_START 0x393bu
#define DCN_LOCAL_END 0x393cu
#define DCN_LOCAL_LOCK 0x393du
#define DCHVM_CLK_CTRL 0x6ac5u
#define DCHVM_MEM_CTRL 0x6ac6u
#define DCHVM_RIOMMU_CTRL0 0x6ac7u
#define DCHVM_RIOMMU_STAT0 0x6ac8u
static uint32_t dcn_saved[10];
static volatile int dcn_armed;
static void dcn_restore(void)
{
   if (!dcn_armed) return;
   wr_abs(HUBP0_ADDR_HI, dcn_saved[0]); wr_abs(HUBP0_ADDR_LO, dcn_saved[1]);
   usleep(50000);   // past the next vsync: the pipe is back on the old frame before the apertures go
   wr_abs(HUBP0_TLB_CNTL, dcn_saved[2]); wr_abs(HUBP0_APT_LOW, dcn_saved[3]); wr_abs(HUBP0_APT_HIGH, dcn_saved[4]);
   wr_abs(DCN_AGP_BOT, dcn_saved[5]); wr_abs(DCN_AGP_TOP, dcn_saved[6]); wr_abs(DCN_AGP_BASE, dcn_saved[7]);
   wr_abs(DCN_LOCAL_START, dcn_saved[8]); wr_abs(DCN_LOCAL_END, dcn_saved[9]);
   dcn_armed = 0;
}
static void dcn_on_signal(int sig) { dcn_restore(); _exit(128 + sig); }
static int underflow(void) { return (int)((rd_abs(HUBP0_DCHUBP_CNTL) >> 28) & 7); }
static void clear_underflow(void) { wr_abs(HUBP0_DCHUBP_CNTL, rd_abs(HUBP0_DCHUBP_CNTL) | 0x80000000u); }

// dcn35_dchvm_init: DCN 3.5 reaches memory outside the carve-out through DCHVM and the SoC's rIOMMU ("host VM"). The
// firmware leaves it asleep (DCHVM_CTRL0 0, RIOMMU_STAT0 0 under macOS); with it so, the second dcnflip run (Linux's
// pipe apertures, the AGP address taken: SURFACE_EARLIEST_INUSE showed it) still read all ones: white (2026-10-26).
// Linux wakes it once at display init and leaves it on; so does this (the dynamic clock gating it re-enables at the
// end is the same). The rIOMMU port setting Linux then sends DMCUB (set_riommu_pctrl 0x20) is not sent here.
static int dchvm_init(void)
{
   wr_abs(DCHVM_CTRL0, rd_abs(DCHVM_CTRL0) | 1u);                      // HOSTVM_INIT_REQ
   int active = 0;
   for (int i = 0; i < 100 && !active; i++) { active = rd_abs(DCHVM_RIOMMU_STAT0) & 1u; if (!active) usleep(5); }
   if (active) {
      wr_abs(DCHVM_MEM_CTRL, (rd_abs(DCHVM_MEM_CTRL) | 1u) & ~4u);      // GPUVMRET_PWR_REQ_DIS 1, FORCE_REQ 0
      wr_abs(DCHVM_CLK_CTRL, rd_abs(DCHVM_CLK_CTRL) | 0x33u);           // DISPCLK/DCFCLK R/G gate disable
      wr_abs(DCHVM_RIOMMU_CTRL0, rd_abs(DCHVM_RIOMMU_CTRL0) | 2u);     // HOSTVM_POWERSTATUS
      usleep(5);
      wr_abs(DCHVM_RIOMMU_CTRL0, rd_abs(DCHVM_RIOMMU_CTRL0) | 1u);     // HOSTVM_PREFETCH_REQ
      int done = 0;
      for (int i = 0; i < 100 && !done; i++) { done = (rd_abs(DCHVM_RIOMMU_STAT0) >> 1) & 1u; if (!done) usleep(5); }
      wr_abs(DCHVM_MEM_CTRL, rd_abs(DCHVM_MEM_CTRL) & ~1u);             // memory power requests back on
      wr_abs(DCHVM_CLK_CTRL, rd_abs(DCHVM_CLK_CTRL) & ~0x33u);          // dynamic clock gating back on
      printf("dcnflip: DCHVM: rIOMMU active, prefetch %s; CTRL0 %x STAT0 %x MEM %x CLK %x RIOMMU_CTRL0 %x\n", done ? "done" : "NOT done",
             rd_abs(DCHVM_CTRL0), rd_abs(DCHVM_RIOMMU_STAT0), rd_abs(DCHVM_MEM_CTRL), rd_abs(DCHVM_CLK_CTRL), rd_abs(DCHVM_RIOMMU_CTRL0));
   } else {
      printf("dcnflip: DCHVM: rIOMMU did NOT become active (STAT0 %x) - Linux goes on without it too\n", rd_abs(DCHVM_RIOMMU_STAT0));
   }
   return active;
}

static int dcnflip(int seconds)
{
   const uint32_t hi = rd_abs(HUBP0_ADDR_HI), lo = rd_abs(HUBP0_ADDR_LO), pitchReg = rd_abs(HUBP0_PITCH), vp = rd_abs(HUBP0_VIEWPORT);
   const uint32_t W = (vp & 0x3fff), H = (vp >> 16) & 0x3fff, pitchPx = (pitchReg & 0x3fff) + 1;
   printf("dcnflip: pipe 0 scans %u:%08x, %ux%u, pitch %u px; DCN AGP %x/%x/%x, pipe aperture %x..%x, TLB_CNTL %x\n",
          hi, lo, W, H, pitchPx, rd_abs(DCN_AGP_BOT), rd_abs(DCN_AGP_TOP), rd_abs(DCN_AGP_BASE),
          rd_abs(HUBP0_APT_LOW), rd_abs(HUBP0_APT_HIGH), rd_abs(HUBP0_TLB_CNTL));
   if (hi != 0x80 || lo != 0 || !W || !H || pitchPx < W) { printf("dcnflip: STOP: pipe 0 is not on the firmware's frame at 0x80:00000000\n"); return 1; }
   if (rd_abs(DCN_AGP_BOT) || rd_abs(DCN_AGP_TOP)) { printf("dcnflip: STOP: the DCN AGP aperture is in use already\n"); return 1; }
   const uint64_t bytes = (uint64_t)pitchPx * 4 * H;
   uint64_t phys = 0, len = 0;
   if (sm(1, bytes, 0, &phys, &len) || !phys) { printf("dcnflip: STOP: no contiguous %llu-byte buffer in system RAM\n", (unsigned long long)bytes); return 1; }
   printf("dcnflip: buffer %llu bytes at system physical 0x%llx\n", (unsigned long long)len, (unsigned long long)phys);
   if (phys + len > 0x8C1000000ull) { printf("dcnflip: STOP: buffer above the aperture's reach\n"); sm(4, 0, 0, NULL, NULL); return 1; }
   static const uint32_t band[5] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFF00FFu};   // BGRA words: A R G B
   for (uint32_t y = 0; y < 5 * 48 && y < H; y++)
      for (uint32_t x = 0; x < W; x++) sm(3, ((uint64_t)y * pitchPx + x) * 4, band[y / 48], NULL, NULL);
   uint64_t chk = 0, v = 0;
   sm(2, ((uint64_t)100 * pitchPx + 10) * 4, 0, &chk, &v);
   printf("dcnflip: bands written (row 100, the blue band, reads back 0x%08x, want ff0000ff)\n", (uint32_t)v);

   const uint32_t agpBot = 0x8100, agpTop = 0x89c0;   // MC 0x81_0000_0000 .. 0x89_C0FF_FFFF = system physical 0 .. 0x8_C0FF_FFFF
   const uint32_t aptLow = 0x200000;                  // the FB's start, 0x80_0000_0000 >> 18, as the pipe has it under Linux
   const uint32_t aptHigh = (uint32_t)((((uint64_t)agpTop << 24) | 0xFFFFFFull) >> 18);
   const uint64_t mc = ((uint64_t)agpBot << 24) + phys;
   dcn_saved[0] = hi; dcn_saved[1] = lo; dcn_saved[2] = rd_abs(HUBP0_TLB_CNTL); dcn_saved[3] = rd_abs(HUBP0_APT_LOW);
   dcn_saved[4] = rd_abs(HUBP0_APT_HIGH); dcn_saved[5] = rd_abs(DCN_AGP_BOT); dcn_saved[6] = rd_abs(DCN_AGP_TOP); dcn_saved[7] = rd_abs(DCN_AGP_BASE);
   dcn_saved[8] = rd_abs(DCN_LOCAL_START); dcn_saved[9] = rd_abs(DCN_LOCAL_END);
   dcn_armed = 1;
   signal(SIGINT, dcn_on_signal); signal(SIGTERM, dcn_on_signal); signal(SIGHUP, dcn_on_signal);
   printf("dcnflip: DCHVM before: CTRL0 %x STAT0 %x MEM %x CLK %x RIOMMU_CTRL0 %x\n", rd_abs(DCHVM_CTRL0),
          rd_abs(DCHVM_RIOMMU_STAT0), rd_abs(DCHVM_MEM_CTRL), rd_abs(DCHVM_CLK_CTRL), rd_abs(DCHVM_RIOMMU_CTRL0));
   if (getenv("DCNFLIP_DCHVM") && *getenv("DCNFLIP_DCHVM") == '1' && !(rd_abs(DCHVM_RIOMMU_STAT0) & 1u)) dchvm_init();
   // DCN sends a physical address inside DCN_VM_LOCAL_HBM_ADDRESS_START..END (16 MB units) straight to memory and any
   // other one to DCHVM and the rIOMMU, which macOS never turns on (the third run: "rIOMMU did NOT become active",
   // white). The firmware marks only the carve-out local (0x780..0x87f); on an APU the rest of DRAM is the same
   // memory, so the range is widened to start at the buffer (not locked: DCN_VM_LOCAL_HBM_ADDRESS_LOCK_CNTL 0).
   const uint32_t lockc = rd_abs(DCN_LOCAL_LOCK), lstart = rd_abs(DCN_LOCAL_START), lend = rd_abs(DCN_LOCAL_END);
   const uint32_t wantStart = (uint32_t)(phys >> 24), wantEnd = (uint32_t)((phys + len - 1) >> 24);
   printf("dcnflip: DCN local memory %x..%x (lock %x); the buffer is %x..%x\n", lstart, lend, lockc, wantStart, wantEnd);
   if (getenv("DCNFLIP_LOCAL") && *getenv("DCNFLIP_LOCAL") == '0') {
      printf("dcnflip: (DCNFLIP_LOCAL=0: local range left as it is)\n");
   } else {
      if (wantStart < lstart) wr_abs(DCN_LOCAL_START, wantStart);
      if (wantEnd > lend) wr_abs(DCN_LOCAL_END, wantEnd);
      printf("dcnflip: DCN local memory now %x..%x\n", rd_abs(DCN_LOCAL_START), rd_abs(DCN_LOCAL_END));
   }
   clear_underflow();
   wr_abs(DCN_AGP_BASE, 0); wr_abs(DCN_AGP_TOP, agpTop); wr_abs(DCN_AGP_BOT, agpBot);
   wr_abs(HUBP0_APT_LOW, aptLow); wr_abs(HUBP0_APT_HIGH, aptHigh);
   wr_abs(HUBP0_TLB_CNTL, 0x19);   // ENABLE_L1_TLB, SYSTEM_ACCESS_MODE 3 (hubp3_set_vm_system_aperture_settings)
   usleep(50000);
   printf("dcnflip: set: DCN AGP %x/%x/%x, pipe aperture %x..%x, TLB_CNTL %x; on the old frame: underflow %d\n",
          rd_abs(DCN_AGP_BOT), rd_abs(DCN_AGP_TOP), rd_abs(DCN_AGP_BASE), rd_abs(HUBP0_APT_LOW), rd_abs(HUBP0_APT_HIGH),
          rd_abs(HUBP0_TLB_CNTL), underflow());
   if (underflow()) { printf("dcnflip: STOP: the settings alone disturbed the old frame\n"); dcn_restore(); sm(4, 0, 0, NULL, NULL); return 1; }
   printf("dcnflip: flipping to MC 0x%llx for %d s - the top of the panel should show five colour bands\n", (unsigned long long)mc, seconds);
   fflush(stdout);
   wr_abs(HUBP0_ADDR_HI, (uint32_t)(mc >> 32)); wr_abs(HUBP0_ADDR_LO, (uint32_t)mc);   // low last: it latches the flip
   usleep(100000);
   const uint32_t inHi = rd_abs(HUBP0_INUSE_HI) & 0xffff, inLo = rd_abs(HUBP0_INUSE_LO);
   const int uf = underflow();
   sleep(seconds > 0 && seconds < 30 ? seconds : 4);
   const int uf2 = underflow();
   dcn_restore();
   usleep(50000);
   printf("dcnflip: during: in use %x:%08x (%s), underflow %d after 0.1 s, %d at the end\n", inHi, inLo,
          ((uint64_t)inHi << 32 | inLo) == mc ? "the buffer" : "NOT the buffer", uf, uf2);
   printf("dcnflip: back: pipe scans %x:%08x, in use %x:%08x, DCN AGP bot %x, pipe TLB_CNTL %x, underflow %d\n",
          rd_abs(HUBP0_ADDR_HI), rd_abs(HUBP0_ADDR_LO), rd_abs(HUBP0_INUSE_HI) & 0xffff, rd_abs(HUBP0_INUSE_LO),
          rd_abs(DCN_AGP_BOT), rd_abs(HUBP0_TLB_CNTL), underflow());
   clear_underflow();
   sm(4, 0, 0, NULL, NULL);
   return 0;
}

// sdmatest [frames]: can SDMA0 copy WindowServer-sized frames into the carve-out, and can the display flip between two
// frames there? The way to a CPU-free frame on the 880M (docs/strix-point.md): build 34/35 copied on the compute queue
// WindowServer's Metal runs on, and a stuck copy took WindowServer down with it. SDMA is an engine of its own (Linux's
// TTM moves buffers with it, sdma_v6_0_buffer_funcs: one COPY_LINEAR packet takes up to 1 GiB), so a hang there costs
// only the copies. Nothing the running system uses is touched: SDMA0 queue 0 (never started by the bring-up; Linux
// programs the kernel queue by registers, sdma_v6_0_gfx_resume_instance), VMID 7 (the kext lends 8..15), VRAM
// 0x6000000..0xC800000 (nothing lives there: fw_buf ends at 0x5000000, the TMR sits 512 MiB below the top), TLB
// invalidation engine 16 (the kext uses 17). Every step is checked before the next one runs, and everything is put back.
//   S1 queue up, one FENCE                       S2 VMID 0: CONSTANT_FILL + COPY_LINEAR VRAM->VRAM, checked
//   S3 VMID 7 IB: system RAM -> carve-out        S4 full frames (1920x1200, 3072x1920): system RAM -> carve-out as
//      (the kernel's way: page tables), checked      local memory / as snooped system pages, and VRAM -> VRAM, timed
//   S5 flip: pipe 0 shows a second frame in the carve-out (the firmware's frame + five bands) for 3 s, and back
// SDMA_FLIP=0 skips S5, SDMA_MT=<n> sets the local PTEs' MTYPE.
#define SD0(o) (RDNA3_GC_SEG0 + (o))          // GC base 0 (SDMA0 registers: the GC 11.0 offsets on 11.5 too)
#define SD1(o) (RDNA3_GC_SEG1 + (o))          // GC base 1 (the "HYP_DEC" range)
#define SDR_UCODE_VERSION SD0(0x0d)
#define SDR_CNTL          SD0(0x1c)
#define SDR_SEM_WAIT_FAIL SD0(0x22)
#define SDR_STATUS        SD0(0x25)
#define SDR_STATUS1       SD0(0x26)
#define SDR_FREEZE        SD0(0x2b)
#define SDR_STATUS2       SD0(0x38)
#define SDR_UTCL1_CNTL    SD0(0x3c)
#define SDR_UTCL1_PAGE    SD0(0x3f)
#define SDR_STATUS3       SD0(0x4c)
#define SDR_ERROR_LOG     SD0(0x50)
#define SDR_RB_CNTL       SD0(0x80)
#define SDR_RB_BASE       SD0(0x81)
#define SDR_RB_BASE_HI    SD0(0x82)
#define SDR_RB_RPTR       SD0(0x83)
#define SDR_RB_RPTR_HI    SD0(0x84)
#define SDR_RB_WPTR       SD0(0x85)
#define SDR_RB_WPTR_HI    SD0(0x86)
#define SDR_RPTR_ADDR_HI  SD0(0x88)
#define SDR_RPTR_ADDR_LO  SD0(0x89)
#define SDR_IB_CNTL       SD0(0x8a)
#define SDR_IB_RPTR       SD0(0x8b)
#define SDR_IB_BASE_LO    SD0(0x8d)
#define SDR_IB_SIZE       SD0(0x8f)
#define SDR_CTX_STATUS    SD0(0x91)
#define SDR_DOORBELL      SD0(0x92)
#define SDR_PREEMPT       SD0(0xb0)
#define SDR_POLL_ADDR_HI  SD0(0xb2)
#define SDR_POLL_ADDR_LO  SD0(0xb3)
#define SDR_MINOR_PTR_UPD SD0(0xb5)
#define SDR_F32_CNTL      SD1(0x589a)
#define SDR_GRBM_SOFT_RST SD0(0xda8)
// VRAM (carve-out offsets) the test uses, all below 200 MiB so the BAR reaches them
#define SD_RING  0x6000000ull                 // 64 KiB ring
#define SD_RDW   16384u
#define SD_WB    0x6010000ull                 // +0 rptr write-back, +0x10 wptr shadow, +0x20 fence, +0x40.. timestamps
#define SD_IB    0x6020000ull                 // 64 KiB of IBs (VMID 7 sees them at SD_VA_IB)
#define SD_PT    0x6100000ull                 // VMID 7 page tables, 256 pages
#define SD_PTMAX 256
#define SD_TA    0x7000000ull                 // two 1 MiB test areas
#define SD_TB    0x7100000ull
#define SD_DST   0x8000000ull                 // full-frame destination, 32 MiB
#define SD_FLIPF 0xA800000ull                 // the flip test's frame, 32 MiB (ends 0xC800000)
#define SD_VMID  7
#define SD_VA_IB  0x100000000ull              // VMID 7: IB pages, the system buffer, the destination twice
#define SD_VA_SRC 0x200000000ull
#define SD_VA_DL  0x300000000ull              // SD_DST as local memory
#define SD_VA_DS  0x400000000ull              // SD_DST as snooped system pages (build 35's default for the scan-out)
static uint32_t sd_wp, sd_seq, sd_f32_was, sd_ctx_saved[7];
static int sd_ctx_armed, sd_ptn, sd_flip_armed;
static uint64_t sd_ptm[SD_PTMAX][512];        // what the page tables hold (they live in VRAM; this mirror saves reads)
static double sd_now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static void sd_hdp(void) { if (hdp_flush_dword) { wr_abs(hdp_flush_dword, 0); (void)rd_abs(SD0(0xda4)); } }
static void sd_put(uint32_t v) { vw(SD_RING + (uint64_t)(sd_wp & (SD_RDW - 1)) * 4, v); sd_wp++; }
static void sd_kick(void)
{
   sd_hdp();
   vw(SD_WB + 0x10, sd_wp * 4); vw(SD_WB + 0x14, 0);
   sd_hdp();
   wr_abs(SDR_RB_WPTR, sd_wp * 4); wr_abs(SDR_RB_WPTR_HI, 0);   // bytes, as sdma_v6_0_ring_set_wptr
}
static uint32_t sd_fence(void)                  // FENCE (MTYPE UC) of the next sequence number to SD_WB + 0x20
{
   const uint64_t a = MC(SD_WB + 0x20);
   sd_put(5u | (3u << 16)); sd_put((uint32_t)a); sd_put((uint32_t)(a >> 32)); sd_put(++sd_seq);
   return sd_seq;
}
static void sd_gcr(void)                        // GCR_REQ: GL2 write-back + invalidate, as sdma_v6_0_ring_emit_mem_sync
{
   const uint32_t g = (1u << 15) | (1u << 14) | (1u << 9) | (1u << 8) | (1u << 7) | (1u << 5) | 1u;
   sd_put(17u); sd_put(0); sd_put((g & 0xffff) << 16); sd_put((g >> 16) & 7); sd_put(0);
}
static void sd_ts(int slot)                     // TIMESTAMP GET_GLOBAL (64 bits) to SD_WB + 0x40 + 8 * slot
{
   const uint64_t a = MC(SD_WB + 0x40 + 8 * (uint64_t)slot);
   sd_put(13u | (2u << 8)); sd_put((uint32_t)a); sd_put((uint32_t)(a >> 32));
}
static uint64_t sd_ts_read(int slot) { return vr(SD_WB + 0x40 + 8 * slot) | ((uint64_t)vr(SD_WB + 0x44 + 8 * slot) << 32); }
static void sd_emit_copy(void (*put)(uint32_t), uint64_t src, uint64_t dst, uint64_t bytes)
{
   put(1u); put((uint32_t)(bytes - 1)); put(0);
   put((uint32_t)src); put((uint32_t)(src >> 32)); put((uint32_t)dst); put((uint32_t)(dst >> 32));
}
// FILLSIZE 2 (bits 31:30): dword fill. 0 is a byte fill (libdrm's SDMA_CONSTANT_FILL_EXTRA_SIZE(2) for 0xdeadbeaf; Linux's
// sdma_v6_0_emit_fill_buffer leaves 0 and fills with zeros).
static void sd_emit_fill(void (*put)(uint32_t), uint64_t dst, uint32_t v, uint64_t bytes)
{
   put(11u | (2u << 30)); put((uint32_t)dst); put((uint32_t)(dst >> 32)); put(v); put((uint32_t)(bytes - 1));
}
static void sd_dump(const char *why)
{
   printf("sdma: %s: F32_CNTL %08x STATUS %08x/%08x/%08x/%08x ERROR_LOG %08x CNTL %08x\n", why, rd_abs(SDR_F32_CNTL),
          rd_abs(SDR_STATUS), rd_abs(SDR_STATUS1), rd_abs(SDR_STATUS2), rd_abs(SDR_STATUS3), rd_abs(SDR_ERROR_LOG), rd_abs(SDR_CNTL));
   printf("sdma:   RB_CNTL %08x base %08x:%08x rptr %u (wb %u) wptr %u IB_CNTL %08x IB_RPTR %u IB_BASE %08x IB_SIZE %u CTX %08x\n",
          rd_abs(SDR_RB_CNTL), rd_abs(SDR_RB_BASE_HI), rd_abs(SDR_RB_BASE), rd_abs(SDR_RB_RPTR), vr(SD_WB), rd_abs(SDR_RB_WPTR),
          rd_abs(SDR_IB_CNTL), rd_abs(SDR_IB_RPTR), rd_abs(SDR_IB_BASE_LO), rd_abs(SDR_IB_SIZE), rd_abs(SDR_CTX_STATUS));
   const uint32_t st = rd(SEG0(0x15c8)), hi = rd(SEG0(0x15ca)), lo = rd(SEG0(0x15c9));
   printf("sdma:   VM fault status %08x addr %x:%08x; fence %u (want %u)\n", st, hi, lo, vr(SD_WB + 0x20), sd_seq);
}
// Wait for fence `seq`; on a timeout the engine is stopped and reset the way sdma_v6_0_soft_reset does it (SDMA0 only).
static int sd_wait(uint32_t seq, double limit_ms, double *ms)
{
   const double t0 = sd_now_ms();
   for (;;) {
      const uint32_t f = vr(SD_WB + 0x20);
      if ((int32_t)(f - seq) >= 0) { if (ms) *ms = sd_now_ms() - t0; return 0; }
      if (sd_now_ms() - t0 > limit_ms) break;
   }
   sd_dump("TIMEOUT");
   wr_abs(SDR_RB_CNTL, rd_abs(SDR_RB_CNTL) & ~1u); wr_abs(SDR_IB_CNTL, rd_abs(SDR_IB_CNTL) & ~1u);
   wr_abs(SDR_FREEZE, rd_abs(SDR_FREEZE) | 0x10u);                       // SDMA0_FREEZE.FREEZE
   wr_abs(SDR_F32_CNTL, rd_abs(SDR_F32_CNTL) | 1u | (1u << 13));        // HALT, TH1_RESET
   wr_abs(SDR_PREEMPT, 0); usleep(100);
   wr_abs(SDR_GRBM_SOFT_RST, 1u << 23); (void)rd_abs(SDR_GRBM_SOFT_RST); usleep(100);
   wr_abs(SDR_GRBM_SOFT_RST, 0); (void)rd_abs(SDR_GRBM_SOFT_RST); usleep(100);
   wr_abs(SDR_FREEZE, rd_abs(SDR_FREEZE) & ~0x10u);
   printf("sdma: engine halted and soft-reset (SDMA0 only); F32_CNTL now %08x\n", rd_abs(SDR_F32_CNTL));
   return -1;
}
// VMID 7's tables: 4 levels (39/30/21/12) in VRAM, PDE = physical | VALID, as the GART's PDB.
static int sd_ptalloc(void)
{
   if (sd_ptn >= SD_PTMAX) { printf("sdma: STOP: out of page-table pages\n"); exit(1); }
   memset(sd_ptm[sd_ptn], 0, sizeof(sd_ptm[0]));   // the whole SD_PT range is zeroed by SDMA in S2
   return sd_ptn++;
}
static void sd_ptset(int pg, unsigned idx, uint64_t v)
{
   sd_ptm[pg][idx] = v;
   const uint64_t off = SD_PT + (uint64_t)pg * 4096 + idx * 8;
   vw(off, (uint32_t)v); vw(off + 4, (uint32_t)(v >> 32));
}
static int sd_child(int pg, unsigned idx)
{
   if (!(sd_ptm[pg][idx] & 1)) sd_ptset(pg, idx, (FB_PA + SD_PT + (uint64_t)sd_ptalloc() * 4096) | 1);
   return (int)(((sd_ptm[pg][idx] & 0x0000FFFFFFFFF000ull) - FB_PA - SD_PT) / 4096);
}
static void sd_map(uint64_t va, uint64_t pte)
{
   const int l1 = sd_child(0, (va >> 39) & 511), l2 = sd_child(l1, (va >> 30) & 511), pt = sd_child(l2, (va >> 21) & 511);
   sd_ptset(pt, (va >> 12) & 511, pte);
}
static void sd_tlb(void)
{
   sd_hdp();
   wr(SEG0(0x16ab + 16), 0x00F80000u | (1u << SD_VMID) | 1u);
   for (int i = 0; i < 1000 && !(rd(SEG0(0x16bd + 16)) & (1u << SD_VMID)); i++) usleep(1);
}
static const uint32_t sd_ctx_reg[7] = {0x16f3 + 2 * SD_VMID, 0x16f4 + 2 * SD_VMID, 0x1688 + SD_VMID, 0x1713 + 2 * SD_VMID,
                                       0x1714 + 2 * SD_VMID, 0x1733 + 2 * SD_VMID, 0x1734 + 2 * SD_VMID};
static void sd_ctx_restore(void)
{
   if (!sd_ctx_armed) return;
   for (int i = 0; i < 7; i++) wr(SEG0(sd_ctx_reg[i]), sd_ctx_saved[i]);
   sd_tlb();
   sd_ctx_armed = 0;
}
static void sd_teardown(void)
{
   if (sd_flip_armed) { wr_abs(HUBP0_ADDR_HI, 0x80); wr_abs(HUBP0_ADDR_LO, 0); sd_flip_armed = 0; }
   wr_abs(SDR_RB_CNTL, rd_abs(SDR_RB_CNTL) & ~1u); wr_abs(SDR_IB_CNTL, rd_abs(SDR_IB_CNTL) & ~1u);
   if (sd_f32_was & 1u) wr_abs(SDR_F32_CNTL, rd_abs(SDR_F32_CNTL) | 1u);   // halted before: halted again
   sd_ctx_restore();
   sm(4, 0, 0, NULL, NULL);
}
static void sd_on_signal(int sig) { sd_teardown(); _exit(128 + sig); }
// One IB in VMID 7 (dwords at SD_IB, padded to 8), then GCR + FENCE in the ring; returns the fence's sequence.
static uint32_t sd_ibw;
static void sd_ibput(uint32_t v) { vw(SD_IB + (uint64_t)sd_ibw * 4, v); sd_ibw++; }
static uint32_t sd_submit_ib(int ts)
{
   while (sd_ibw & 7) sd_ibput(0);                        // NOP padding (sdma_v6_0_ring_pad_ib)
   if (ts) sd_ts(0);
   while (((sd_wp + 6) & 7) != 0) sd_put(0);              // the IB packet ends on an 8-dword boundary
   sd_put(4u | ((uint32_t)SD_VMID << 16)); sd_put((uint32_t)SD_VA_IB); sd_put((uint32_t)(SD_VA_IB >> 32));
   sd_put(sd_ibw); sd_put(0); sd_put(0);
   if (ts) sd_ts(1);
   sd_gcr();
   const uint32_t s = sd_fence();
   sd_kick();
   sd_ibw = 0;
   return s;
}
static int sd_check(const char *what, uint64_t vram, uint64_t bytes, uint64_t step, uint32_t (*want)(uint64_t))
{
   unsigned bad = 0, n = 0; uint64_t firstBad = 0; uint32_t got = 0, exp = 0;
   for (uint64_t o = 0; o < bytes; o += step, n++) {
      const uint32_t v = vr(vram + o), w = want(o);
      if (v != w && !bad++) { firstBad = o; got = v; exp = w; }
   }
   const uint32_t lv = vr(vram + bytes - 4), lw = want(bytes - 4);
   if (lv != lw && !bad++) { firstBad = bytes - 4; got = lv; exp = lw; }
   n++;
   if (bad) printf("sdma: %s: %u of %u samples WRONG, first at +0x%llx: 0x%08x, want 0x%08x\n", what, bad, n,
                   (unsigned long long)firstBad, got, exp);
   else printf("sdma: %s: %u samples right\n", what, n);
   return bad ? -1 : 0;
}
static uint32_t sd_pat(uint64_t o) { return 0x9E3779B1u * (uint32_t)(o / 4 + 1); }
static uint32_t sd_fillv(uint64_t o) { (void)o; return 0x5A5A0000u; }
static uint64_t sd_sys_phys, sd_sys_len;
// the system buffer: zero but for a marker every 64 KiB and one in its last word
static uint32_t sd_sysmark(uint64_t o) { return (o % 65536 == 0 || o == sd_sys_len - 4) ? 0xC0DE0000u + (uint32_t)(o >> 16) : 0; }

static int sdmatest(int frames)
{
   if (frames < 1 || frames > 100) frames = 10;
   const int mt = getenv("SDMA_MT") ? atoi(getenv("SDMA_MT")) : 0;
   const uint64_t pteLocal = 0x71ull | ((uint64_t)mt << 48), pteSys = 0x77ull | (4ull << 48);   // the GART's / the kext's flags
   // S0: where SDMA0 stands, and whether the VRAM the test uses is reachable through the BAR
   sd_f32_was = rd_abs(SDR_F32_CNTL);
   printf("sdma: S0 ucode version %08x, F32_CNTL %08x (%s), RB_CNTL %08x, STATUS %08x, HDP flush reg 0x%x, carve-out at 0x%llx\n",
          rd_abs(SDR_UCODE_VERSION), sd_f32_was, (sd_f32_was & 1) ? "halted" : "running", rd_abs(SDR_RB_CNTL), rd_abs(SDR_STATUS),
          hdp_flush_dword, (unsigned long long)FB_PA);
   if ((rd_abs(SDR_RB_CNTL) & 1u) && !env_on("SDMA_FORCE")) { printf("sdma: STOP: queue 0 is enabled already (someone uses it)\n"); return 1; }
   vw(SD_FLIPF + (32u << 20) - 4, 0x13579BDFu);
   if (vr(SD_FLIPF + (32u << 20) - 4) != 0x13579BDFu) { printf("sdma: STOP: VRAM at 0x%llx is out of the BAR's reach\n", (unsigned long long)(SD_FLIPF + (32u << 20))); return 1; }
   signal(SIGINT, sd_on_signal); signal(SIGTERM, sd_on_signal); signal(SIGHUP, sd_on_signal);

   // S1: queue 0 as sdma_v6_0_gfx_resume_instance programs it, no doorbell (wptr by register), then one FENCE
   for (int i = 0; i < 0x30; i += 4) vw(SD_WB + i, 0);
   for (uint32_t i = 0; i < 64; i++) vw(SD_RING + i * 4, 0);
   wr_abs(SDR_SEM_WAIT_FAIL, 0);
   uint32_t rb = rd_abs(SDR_RB_CNTL) & ~1u;
   rb = (rb & ~(0x1fu << 1)) | (14u << 1);                         // RB_SIZE: log2(16384 dwords)
   rb |= 1u << 23;                                                 // RB_PRIV
   rb &= ~(0xfu << 24);                                            // RB_VMID 0
   wr_abs(SDR_RB_CNTL, rb);
   wr_abs(SDR_RB_RPTR, 0); wr_abs(SDR_RB_RPTR_HI, 0); wr_abs(SDR_RB_WPTR, 0); wr_abs(SDR_RB_WPTR_HI, 0);
   wr_abs(SDR_POLL_ADDR_LO, (uint32_t)MC(SD_WB + 0x10)); wr_abs(SDR_POLL_ADDR_HI, (uint32_t)(MC(SD_WB + 0x10) >> 32));
   wr_abs(SDR_RPTR_ADDR_HI, (uint32_t)(MC(SD_WB) >> 32)); wr_abs(SDR_RPTR_ADDR_LO, (uint32_t)MC(SD_WB) & ~3u);
   rb |= (1u << 12) | (1u << 11); rb &= ~(1u << 8);                // RPTR_WRITEBACK, F32_WPTR_POLL; no WPTR_POLL
   wr_abs(SDR_RB_BASE, (uint32_t)(MC(SD_RING) >> 8)); wr_abs(SDR_RB_BASE_HI, (uint32_t)(MC(SD_RING) >> 40));
   wr_abs(SDR_MINOR_PTR_UPD, 1);
   wr_abs(SDR_RB_WPTR, 0); wr_abs(SDR_RB_WPTR_HI, 0);
   wr_abs(SDR_DOORBELL, rd_abs(SDR_DOORBELL) & ~(1u << 28));
   wr_abs(SDR_MINOR_PTR_UPD, 0);
   wr_abs(SDR_UTCL1_CNTL, (rd_abs(SDR_UTCL1_CNTL) & ~0x61fu) | (3u << 9) | 9u);    // RESP_MODE (0x600) 3, REDO_DELAY (0x1f) 9
   wr_abs(SDR_UTCL1_PAGE, (rd_abs(SDR_UTCL1_PAGE) & 0xFF0FFFu) | (2u << 12) | (3u << 14) | (1u << 24));   // RD NOA, WR BYPASS, LLC_NOALLOC
   wr_abs(SDR_F32_CNTL, rd_abs(SDR_F32_CNTL) & ~(1u | (1u << 13)));                                 // unhalt (HALT, TH1_RESET 0)
   wr_abs(SDR_RB_CNTL, rb | 1u);
   wr_abs(SDR_IB_CNTL, rd_abs(SDR_IB_CNTL) | 1u);
   sd_wp = 0; sd_seq = 0;
   double ms = 0;
   uint32_t s = sd_fence(); sd_kick();
   if (sd_wait(s, 500, &ms)) { printf("sdma: S1 FAILED: the first FENCE never landed\n"); sd_teardown(); return 2; }
   printf("sdma: S1 passed: queue 0 runs (fence in %.2f ms, rptr write-back %u bytes)\n", ms, vr(SD_WB));

   // S2: VMID 0, MC addresses: fill A with a constant, copy A -> B, and zero the page-table pages for S3
   sd_emit_fill(sd_put, MC(SD_TA), 0x5A5A0000u, 1u << 20);
   sd_emit_fill(sd_put, MC(SD_TB), 0, 1u << 20);
   sd_emit_fill(sd_put, MC(SD_PT), 0, (uint64_t)SD_PTMAX * 4096);
   sd_gcr(); s = sd_fence(); sd_kick();
   if (sd_wait(s, 500, &ms)) { printf("sdma: S2 FAILED: CONSTANT_FILL did not finish\n"); sd_teardown(); return 2; }
   if (sd_check("S2 fill", SD_TA, 1u << 20, 4096 + 4, sd_fillv)) { sd_teardown(); return 2; }
   for (uint64_t o = 0; o < (1u << 20); o += 4096 + 4) vw(SD_TA + o, sd_pat(o));
   vw(SD_TA + (1u << 20) - 4, sd_pat((1u << 20) - 4));
   sd_emit_copy(sd_put, MC(SD_TA), MC(SD_TB), 1u << 20);
   sd_gcr(); s = sd_fence(); sd_kick();
   if (sd_wait(s, 500, &ms)) { printf("sdma: S2 FAILED: COPY_LINEAR did not finish\n"); sd_teardown(); return 2; }
   {   // the copy carries both the fill and the CPU's words
      int bad = 0;
      for (uint64_t o = 0; o < (1u << 20); o += 4096 + 4) bad |= vr(SD_TB + o) != sd_pat(o);
      bad |= vr(SD_TB + 8) != 0x5A5A0000u;
      bad |= vr(SD_TB + (1u << 20) - 4) != sd_pat((1u << 20) - 4);
      if (bad) { printf("sdma: S2 FAILED: VRAM->VRAM copy wrong (+0: %08x, +8: %08x)\n", vr(SD_TB), vr(SD_TB + 8)); sd_teardown(); return 2; }
   }
   printf("sdma: S2 passed: CONSTANT_FILL and COPY_LINEAR in VRAM (VMID 0)\n");

   // S3: the kernel's way - an IB in VMID 7 whose page tables map a system buffer and the carve-out
   // physically contiguous for the kext's debug buffer; the 3K frame's 23.6 MB may not be found in a fragmented RAM
   const uint64_t fbig = (uint64_t)3072 * 1920 * 4, fsmall = (uint64_t)1920 * 1200 * 4;
   if ((sm(1, fbig, 0, &sd_sys_phys, &sd_sys_len) || !sd_sys_phys) && (sm(1, fsmall, 0, &sd_sys_phys, &sd_sys_len) || !sd_sys_phys)) {
      printf("sdma: STOP: no contiguous %llu-byte system buffer\n", (unsigned long long)fsmall); sd_teardown(); return 1;
   }
   for (int i = 0; i < 7; i++) sd_ctx_saved[i] = rd(SEG0(sd_ctx_reg[i]));
   sd_ctx_armed = 1;
   sd_ptn = 0; (void)sd_ptalloc();                                 // page 0: the root
   for (uint64_t p = 0; p < 0x10000; p += 4096) sd_map(SD_VA_IB + p, (FB_PA + SD_IB + p) | pteLocal);
   for (uint64_t p = 0; p < sd_sys_len; p += 4096) sd_map(SD_VA_SRC + p, (sd_sys_phys + p) | pteSys);
   for (uint64_t p = 0; p < (32u << 20); p += 4096) {
      sd_map(SD_VA_DL + p, (FB_PA + SD_DST + p) | pteLocal);
      sd_map(SD_VA_DS + p, (FB_PA + SD_DST + p) | pteSys);
   }
   wr(SEG0(sd_ctx_reg[0]), (uint32_t)((FB_PA + SD_PT) | 1)); wr(SEG0(sd_ctx_reg[1]), (uint32_t)((FB_PA + SD_PT) >> 32));
   wr(SEG0(sd_ctx_reg[2]), 0x01fffe07);
   wr(SEG0(sd_ctx_reg[3]), 0); wr(SEG0(sd_ctx_reg[4]), 0);
   wr(SEG0(sd_ctx_reg[5]), 0xffffffff); wr(SEG0(sd_ctx_reg[6]), 0xf);
   sd_tlb();
   printf("sdma: S3 VMID %d: %d page-table pages; system buffer %llu bytes at 0x%llx\n", SD_VMID, sd_ptn,
          (unsigned long long)sd_sys_len, (unsigned long long)sd_sys_phys);
   for (uint64_t o = 0; o < sd_sys_len; o += 65536) sm(3, o, sd_sysmark(o), NULL, NULL);
   sm(3, sd_sys_len - 4, sd_sysmark(sd_sys_len - 4), NULL, NULL);
   fault_clear();
   sd_emit_fill(sd_put, MC(SD_DST), 0xDEADBEEFu, 1u << 20); sd_gcr(); s = sd_fence(); sd_kick();
   if (sd_wait(s, 500, NULL)) { sd_teardown(); return 2; }
   sd_emit_copy(sd_ibput, SD_VA_SRC, SD_VA_DL, 1u << 20);
   s = sd_submit_ib(0);
   if (sd_wait(s, 500, &ms)) { printf("sdma: S3 FAILED: the VMID %d IB never finished\n", SD_VMID); sd_teardown(); return 3; }
   const int bad3 = sd_check("S3 system RAM -> carve-out (1 MiB, VMID 7)", SD_DST, 1u << 20, 4096, sd_sysmark);
   const uint32_t fst = rd(SEG0(0x15c8));
   if (fst) printf("sdma: S3 VM fault status %08x addr %x:%08x\n", fst, rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
   if (bad3) { sd_teardown(); return 3; }
   printf("sdma: S3 passed: system RAM reaches the carve-out through VMID %d page tables (%.2f ms)\n", SD_VMID, ms);

   // S4: whole frames, timed (CPU: submit to fence; GPU: global timestamps around the IB)
   static const struct { uint32_t w, h; } fr[2] = {{1920, 1200}, {3072, 1920}};
   int bad4 = 0;
   for (int k = 0; k < 2; k++) {
      const uint64_t bytes = (uint64_t)fr[k].w * fr[k].h * 4;
      if (bytes > sd_sys_len) { printf("sdma: S4 %ux%u skipped: the system buffer has only %llu bytes\n", fr[k].w, fr[k].h, (unsigned long long)sd_sys_len); continue; }
      for (int mode = 0; mode < 3; mode++) {
         static const char *const kMode[3] = {"system RAM -> carve-out (local PTEs)", "system RAM -> carve-out (snooped system PTEs)",
                                              "carve-out -> carve-out (VMID 0)"};
         double sum = 0, mn = 1e9, mx = 0; uint64_t tsum = 0;
         if (mode < 2) {   // poison the destination, so the check below sees this mode's writes and not the last one's
            sd_emit_fill(sd_put, MC(SD_DST), 0xDEADBEEFu, bytes); sd_gcr(); s = sd_fence(); sd_kick();
            if (sd_wait(s, 1000, NULL)) { sd_teardown(); return 4; }
         }
         for (int i = 0; i < frames; i++) {
            if (mode == 2) { sd_ts(0); sd_emit_copy(sd_put, MC(SD_DST), MC(SD_FLIPF), bytes); sd_ts(1); sd_gcr(); s = sd_fence(); sd_kick(); }
            else { sd_emit_copy(sd_ibput, SD_VA_SRC, mode ? SD_VA_DS : SD_VA_DL, bytes); s = sd_submit_ib(1); }
            if (sd_wait(s, 1000, &ms)) { printf("sdma: S4 FAILED: %ux%u %s timed out\n", fr[k].w, fr[k].h, kMode[mode]); sd_teardown(); return 4; }
            sum += ms; if (ms < mn) mn = ms; if (ms > mx) mx = ms;
            tsum += sd_ts_read(1) - sd_ts_read(0);
         }
         printf("sdma: S4 %ux%u (%.1f MB) %s: %.2f ms avg (min %.2f, max %.2f) = %.1f GB/s; GPU timestamps %llu ticks avg\n",
                fr[k].w, fr[k].h, bytes / 1e6, kMode[mode], sum / frames, mn, mx, bytes / (sum / frames) / 1e6,
                (unsigned long long)(tsum / frames));
         if (mode < 2) {   // every 32 KiB: a marker, then a zero, alternately
            char what[96]; snprintf(what, sizeof what, "S4 %ux%u %s", fr[k].w, fr[k].h, mode ? "snooped" : "local");
            bad4 |= sd_check(what, SD_DST, bytes, 32768, sd_sysmark);
         }
      }
   }
   if (bad4) { printf("sdma: S4 FAILED: a frame arrived wrong\n"); sd_teardown(); return 4; }
   printf("sdma: S4 passed\n");

   // S5: a second frame in the carve-out on the panel: the firmware's frame copied, five bands drawn by SDMA, flipped to
   // for 3 s (the address latches at the next vblank) and back. Pipe 0 never leaves the carve-out, which it reads today.
   if (getenv("SDMA_FLIP") && *getenv("SDMA_FLIP") == '0') { printf("sdma: S5 skipped (SDMA_FLIP=0)\n"); sd_teardown(); return 0; }
   const uint32_t hi = rd_abs(HUBP0_ADDR_HI), lo = rd_abs(HUBP0_ADDR_LO), vp = rd_abs(HUBP0_VIEWPORT), pr = rd_abs(HUBP0_PITCH);
   const uint32_t W = vp & 0x3fff, H = (vp >> 16) & 0x3fff, P = (pr & 0x3fff) + 1;
   const uint64_t fb = (uint64_t)P * 4 * H;
   if (hi != 0x80 || lo != 0 || !W || !H || P < W || fb > (32u << 20)) {
      printf("sdma: S5 skipped: pipe 0 scans %x:%08x %ux%u pitch %u (not the firmware's frame at 0x80:00000000)\n", hi, lo, W, H, P);
      sd_teardown(); return 0;
   }
   static const uint32_t band[5] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFF00FFu};
   sd_emit_copy(sd_put, MC(0), MC(SD_FLIPF), fb);
   for (int b = 0; b < 5; b++) sd_emit_fill(sd_put, MC(SD_FLIPF + (uint64_t)b * 48 * P * 4), band[b], (uint64_t)48 * P * 4);
   sd_gcr(); s = sd_fence(); sd_kick();
   if (sd_wait(s, 500, &ms)) { sd_teardown(); return 5; }
   printf("sdma: S5 frame %ux%u pitch %u prepared in %.2f ms (row 100 reads %08x, want ff0000ff)\n", W, H, P, ms,
          vr(SD_FLIPF + ((uint64_t)100 * P + 10) * 4));
   clear_underflow();
   const uint64_t mc = MC(SD_FLIPF);
   sd_flip_armed = 1;
   const double tf = sd_now_ms();
   wr_abs(HUBP0_ADDR_HI, (uint32_t)(mc >> 32)); wr_abs(HUBP0_ADDR_LO, (uint32_t)mc);   // low last: it latches the flip
   double tl = -1;
   while (sd_now_ms() - tf < 100) {
      if ((rd_abs(HUBP0_INUSE_LO) == (uint32_t)mc) && ((rd_abs(HUBP0_INUSE_HI) & 0xffff) == (uint32_t)(mc >> 32))) { tl = sd_now_ms() - tf; break; }
   }
   const int uf = underflow();
   printf("sdma: S5 flipped to MC 0x%llx: in use after %.2f ms, underflow %d - the panel's top should show five colour bands for 3 s\n",
          (unsigned long long)mc, tl, uf);
   fflush(stdout);
   sleep(3);
   const int uf2 = underflow();
   wr_abs(HUBP0_ADDR_HI, 0x80); wr_abs(HUBP0_ADDR_LO, 0);
   sd_flip_armed = 0;
   usleep(50000);
   printf("sdma: S5 back: pipe scans %x:%08x, in use %x:%08x, underflow %d during, %d after\n", rd_abs(HUBP0_ADDR_HI),
          rd_abs(HUBP0_ADDR_LO), rd_abs(HUBP0_INUSE_HI) & 0xffff, rd_abs(HUBP0_INUSE_LO), uf2, underflow());
   clear_underflow();
   printf("sdma: S5 %s\n", tl >= 0 && !uf && !uf2 ? "passed (the panel showed the bands? say so)" : "FAILED: see above");
   sd_teardown();
   return tl >= 0 && !uf && !uf2 ? 0 : 5;
}

// ---- the eDP link: DPCD over AUX, and DMCUB's VBIOS command tables (the way to 120 Hz) -----------------------------
// AUX channel 0 (the panel's), driven by registers the way Linux DC's dce_aux.c does on this 880M (trace of a 60 <-> 120
// Hz switch, 2026-10-26): take the engine (ARB), write the request bytes into AUX_SW_DATA (action | addr[19:16],
// addr[15:8], addr[7:0], len - 1, data), GO, poll AUX_SW_DONE, read the reply byte by byte (reply code first), give
// the engine back. DMCUB runs under macOS (the GOP started it; INBOX1 holds its 56 commands) and executes VBIOS command
// tables (type 128) the way DC uses it for the transmitter and the panel power.
#define AUXR_SW_CONTROL 0x5411u
#define AUXR_ARB        0x5412u
#define AUXR_INT        0x5413u
#define AUXR_SW_STATUS  0x5414u
#define AUXR_SW_DATA    0x5416u
#define DMCUB_R4_OFF    0x3656u
#define DMCUB_R4_OFF_HI 0x3657u
#define DMCUB_IN1_BASE  0x3694u
#define DMCUB_IN1_SIZE  0x3695u
#define DMCUB_IN1_WPTR  0x3696u
#define DMCUB_IN1_RPTR  0x3697u
#define DMCUB_SCRATCH0  0x36a3u
static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
// One AUX transaction. action 0x80 native write, 0x90 native read. Returns the reply code (0 ACK, 0x10 NACK, 0x20
// DEFER) with the read bytes in rd, or -1 (engine not granted), -2 (no reply / timeout), -3 (receive error).
static int aux_xfer(uint8_t action, uint32_t addr, const uint8_t *wr_data, unsigned len, uint8_t *rd)
{
   const int write = (action & 0x10) == 0;
   wr_abs(AUXR_ARB, 1u << 16);                                   // AUX_SW_USE_AUX_REG_REQ
   double t0 = now_ms();
   while (((rd_abs(AUXR_ARB) >> 2) & 3) != 1) if (now_ms() - t0 > 20) { wr_abs(AUXR_ARB, 1u << 17); return -1; }
   wr_abs(AUXR_INT, rd_abs(AUXR_INT) | 2u);                      // AUX_SW_DONE_ACK
   const unsigned nreq = 4 + (write ? len : 0);
   wr_abs(AUXR_SW_CONTROL, nreq << 16);                          // AUX_SW_WR_BYTES
   const uint8_t hdr[4] = {(uint8_t)(action | ((addr >> 16) & 0xf)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(len - 1)};
   for (unsigned i = 0; i < nreq; i++) {
      const uint8_t b = i < 4 ? hdr[i] : wr_data[i - 4];
      wr_abs(AUXR_SW_DATA, (i == 0 ? 0x80000000u : 0) | ((uint32_t)b << 8));   // index 0 first, then auto-increment
   }
   wr_abs(AUXR_SW_CONTROL, (nreq << 16) | 1u);                   // GO
   uint32_t st = 0; t0 = now_ms();
   while (!((st = rd_abs(AUXR_SW_STATUS)) & 1u)) if (now_ms() - t0 > 20) { wr_abs(AUXR_ARB, 1u << 17); return -2; }
   int ret;
   if (st & ((1u << 7) | (1u << 8) | (1u << 10) | (1u << 12) | (1u << 14) | (0xfu << 17) | (3u << 22))) ret = (st & (1u << 7)) ? -2 : -3;
   else {
      const unsigned n = (st >> 24) & 0x1f;
      uint8_t buf[32] = {0};
      // as dce_aux.c: index 0 and RW once, then sequential reads (the index moves on by itself); the first byte is the
      // reply code. Setting the index before every read (round 40) returned the data shifted by one.
      wr_abs(AUXR_SW_DATA, 0x80000001u);
      for (unsigned i = 0; i < n && i < 32; i++) buf[i] = (uint8_t)(rd_abs(AUXR_SW_DATA) >> 8);
      ret = n ? buf[0] : -2;
      if (!write && rd) memcpy(rd, buf + 1, n > 1 ? (n - 1 < len ? n - 1 : len) : 0);
   }
   wr_abs(AUXR_INT, rd_abs(AUXR_INT) | 2u);
   wr_abs(AUXR_ARB, 1u << 17);                                   // AUX_SW_DONE_USING_AUX_REG
   return ret;
}
// DPCD read/write with Linux's retries on DEFER (and on a lost reply). 0 = ACK.
static int dpcd_read(uint32_t addr, uint8_t *buf, unsigned len)
{
   int r = -2;
   for (int t = 0; t < 7; t++) { r = aux_xfer(0x90, addr, NULL, len, buf); if (r == 0 || r == 0x10) break; usleep(500); }
   return r;
}
static int dpcd_write(uint32_t addr, const uint8_t *buf, unsigned len)
{
   int r = -2;
   for (int t = 0; t < 7; t++) { r = aux_xfer(0x80, addr, buf, len, NULL); if (r == 0 || r == 0x10) break; usleep(500); }
   return r;
}
// One 64-byte command into DMCUB's INBOX1 (region 4 in the carve-out, reached through the kext's window past the
// BAR: PhxBringup 41+), WPTR moved on, waits until DMCUB's RPTR passes it. 0 = executed, -1 busy before, -2 not taken.
static int dmub_send(const uint8_t cmd[64], double limit_ms)
{
   const uint32_t base = rd_abs(DMCUB_IN1_BASE), size = rd_abs(DMCUB_IN1_SIZE), wp = rd_abs(DMCUB_IN1_WPTR);
   double t0 = now_ms();
   while (rd_abs(DMCUB_IN1_RPTR) != wp) if (now_ms() - t0 > 100) return -1;
   const uint64_t r4 = ((uint64_t)rd_abs(DMCUB_R4_OFF_HI) << 32) | rd_abs(DMCUB_R4_OFF);
   const uint64_t off = (r4 - VRAM_MC) + (base - 0x80000000u) + wp;
   for (unsigned i = 0; i < 64; i += 4) vw(off + i, (uint32_t)cmd[i] | (cmd[i + 1] << 8) | (cmd[i + 2] << 16) | ((uint32_t)cmd[i + 3] << 24));
   const uint32_t nwp = (wp + 64) % size;
   wr_abs(DMCUB_IN1_WPTR, nwp);
   t0 = now_ms();
   while (rd_abs(DMCUB_IN1_RPTR) != nwp) if (now_ms() - t0 > limit_ms) return -2;
   return 0;
}
static int hexbytes(const char *s, uint8_t *out, unsigned max)
{
   unsigned n = 0;
   while (*s && n < max) {
      while (*s == ' ' || *s == ':') s++;
      if (!s[0] || !s[1]) break;
      unsigned v; if (sscanf(s, "%2x", &v) != 1) break;
      out[n++] = (uint8_t)v; s += 2;
   }
   return (int)n;
}
static void dmub_state(const char *tag)
{
   const uint64_t r4 = ((uint64_t)rd_abs(DMCUB_R4_OFF_HI) << 32) | rd_abs(DMCUB_R4_OFF);
   printf("dmub %s: SCRATCH0 %08x INBOX1 base %08x size %u wptr %u rptr %u, region 4 at MC %llx\n", tag, rd_abs(DMCUB_SCRATCH0),
          rd_abs(DMCUB_IN1_BASE), rd_abs(DMCUB_IN1_SIZE), rd_abs(DMCUB_IN1_WPTR), rd_abs(DMCUB_IN1_RPTR), (unsigned long long)r4);
}
// ---- 60 <-> 120 Hz: Linux DC's own sequence on this 880M (trace of a GNOME mode switch, 2026-10-26; see
// tools/linux-trace/dc_timeline.py), with what depends on the surface taken from the firmware's 3K@60 setup instead of
// Linux's (its surface is tiled, ours linear):
//   SMU (DCN mailbox): DISPCLK 800 MHz, DPPCLK 782, deep-sleep DCFCLK 55;
//   blank: DPG black, backlight off (VBIOS LVTMA_CONTROL 2 + TRANSMITTER LCD_BLOFF), the DP stream off, OTG off, the
//     DIG front end off, sink D3, TRANSMITTER DISABLE, panel power off (LVTMA 13 + TRANSMITTER POWER_OFF);
//   timing: DP_DTO0_PHASE x2 (the firmware's modulo kept), HUBP DLG/TTU: the firmware's refclk-per-pixel fields x1/2,
//     OTG back on;
//   link: panel power on, ~180 ms, DIG back end on, TRANSMITTER ENABLE at symclk 810000 kHz, sink D0, then link training
//     at LINK_RATE_SET 7 (8.1 Gb/s from the panel's rate table): TPS1, TPS4, the lane settings the panel asks for;
//   stream: DIG FIFO, MSA, DP_VID_M for the new link rate, the stream on, backlight on, DPG off.
// The firmware's 60 Hz values are saved once (/Users/Shared/gpu/hz60.base) and are what `hz 60` puts back.
#define MP1_C2PMSG_67 0x16283u
#define MP1_C2PMSG_83 0x16293u
#define MP1_C2PMSG_91 0x1629bu
static int dcn_smu(uint32_t msg, uint32_t param)
{
   double t0 = now_ms();
   while (rd_abs(MP1_C2PMSG_91) == 0) if (now_ms() - t0 > 50) break;   // the previous one answered
   wr_abs(MP1_C2PMSG_91, 0); wr_abs(MP1_C2PMSG_83, param); wr_abs(MP1_C2PMSG_67, msg);
   uint32_t r = 0; t0 = now_ms();
   while ((r = rd_abs(MP1_C2PMSG_91)) == 0) if (now_ms() - t0 > 200) break;
   printf("hz: SMU msg %u (%u) -> %u, returns %u\n", msg, param, r, rd_abs(MP1_C2PMSG_83));
   return r == 1 ? 0 : -1;
}
static int vb_lvtma(uint8_t action)
{
   uint8_t c[64] = {0x80, 0x0f, 0, 0, action};
   const int r = dmub_send(c, 500);
   if (r) printf("hz: DMCUB LVTMA_CONTROL %u not taken (%d)\n", action, r);
   return r;
}
// VBIOS DIG1_TRANSMITTER_CONTROL as DC sends it (dig_transmitter_control_parameters_v1_7, the bytes of the trace)
static int vb_tx(uint8_t action, uint8_t laneset, uint8_t lanes, uint32_t symclk_10khz, uint8_t conn, uint8_t last)
{
   uint8_t c[64] = {0x80, 0x01, 0x00, 0x3c, 0x00, action, laneset, lanes,
                    (uint8_t)symclk_10khz, (uint8_t)(symclk_10khz >> 8), (uint8_t)(symclk_10khz >> 16), (uint8_t)(symclk_10khz >> 24),
                    0x01, 0x00, conn, last};
   const int r = dmub_send(c, 500);
   if (r) printf("hz: DMCUB TRANSMITTER action %u not taken (%d)\n", action, r);
   return r;
}
static const struct { uint32_t reg; const char *name; int refcyc; } kHzSaved[] = {
   // DLG/TTU (DCN 3.5 pipe 0): REFCYC_* fields scale with the pixel time, DST_Y_* (lines) do not
   {0x3b1c, "HUBPREQ0_REF_FREQ_TO_PIX_FREQ", 1}, {0x3b05, "HUBPREQ0_DST_DIMENSIONS", 1}, {0x3b12, "HUBPREQ0_NOM_PARAMETERS_1", 1},
   {0x3b16, "HUBPREQ0_NOM_PARAMETERS_5", 1}, {0x3b0a, "HUBPREQ0_VBLANK_PARAMETERS_1", 1}, {0x3b0c, "HUBPREQ0_VBLANK_PARAMETERS_3", 1},
   {0x3b1a, "HUBPREQ0_PER_LINE_DELIVERY", 2}, {0x3b19, "HUBPREQ0_PER_LINE_DELIVERY_PRE", 2}, {0x3b03, "HUBPREQ0_BLANK_OFFSET_0", 3},
   {0x3b06, "HUBPREQ0_DST_AFTER_SCALER", 3}, {0x3ae9, "HUBPREQ0_DCN_TTU_QOS_WM", 2}, {0x3aea, "HUBPREQ0_DCN_GLOBAL_TTU_CNTL", 4},
   {0x3aeb, "HUBPREQ0_DCN_SURF0_TTU_CNTL0", 5}, {0x3aec, "HUBPREQ0_DCN_SURF0_TTU_CNTL1", 1},
   // the stream
   {0x0141, "DP_DTO0_PHASE", 0}, {0x0142, "DP_DTO0_MODULO", 0}, {0x55e8, "DP0_DP_VID_M", 0}, {0x55e7, "DP0_DP_VID_N", 0},
};
#define HZ_NSAVED (sizeof(kHzSaved) / sizeof(kHzSaved[0]))
// one register's value for `hz` Hz from its 60 Hz value: the refcyc fields divided by hz/60
static uint32_t hz_scale(int kind, uint32_t v, int hz)
{
   const uint32_t k = (uint32_t)(hz / 60);
   if (k <= 1) return v;
   switch (kind) {
   case 1: return (v & ~0x7fffffu) | ((v & 0x7fffffu) / k);                                          // one field [22:0]
   case 2: return (((v >> 16) & 0x3fffu) / k) << 16 | ((v & 0x3fffu) / k) | (v & 0xc000c000u);      // two 14-bit fields
   case 3: return (v & ~0x1fffu) | ((v & 0x1fffu) / k);                                              // low 13 bits refcyc
   case 4: return (v & ~0xffffffu) | ((v & 0xffffffu) / k);                                          // MIN_TTU_VBLANK
   case 5: return (v & ~0x7fffffu) | ((v & 0x7fffffu) / k);                                          // REFCYC_PER_REQ_DELIVERY
   }
   return v;
}
#define HZ_BASE "/Users/Shared/gpu/hz60.base"
static int hz_base(uint32_t *base, int save)
{
   FILE *f = fopen(HZ_BASE, "r");
   if (f) {
      unsigned n = 0; char name[64]; unsigned a, v;
      while (n < HZ_NSAVED && fscanf(f, "%x %63s %x", &a, name, &v) == 3) { if (a == kHzSaved[n].reg) base[n++] = v; else break; }
      fclose(f);
      if (n == HZ_NSAVED) return 1;
   }
   if (!save) return 0;
   for (unsigned i = 0; i < HZ_NSAVED; i++) base[i] = rd_abs(kHzSaved[i].reg);
   if (!(f = fopen(HZ_BASE, "w"))) return 0;
   for (unsigned i = 0; i < HZ_NSAVED; i++) fprintf(f, "%05x %s %08x\n", kHzSaved[i].reg, kHzSaved[i].name, base[i]);
   fclose(f);
   printf("hz: the firmware's 60 Hz values saved in %s\n", HZ_BASE);
   return 1;
}
// link training at LINK_RATE_SET idx (DP 1.4: TPS1 for clock recovery, TPS4 for equalization, lane settings as the
// panel asks in ADJUST_REQUEST), the PHY's swing/pre-emphasis through VBIOS SETUP_VSEMPH. 0 = trained.
static int hz_train(uint8_t idx, uint32_t symclk)
{
   uint8_t ls = 0, st[6] = {0}, b5[5];
   wr_abs(0x55ee, 0); wr_abs(0x55de, 0); wr_abs(0x55ed, 0); wr_abs(0x55f3, 0x7fffff00);   // source TPS1
   for (int i = 0; i < 4; i++) vb_tx(0x0b, ls, 4, symclk, 0x14, 0xf6);
   if (dpcd_write(0x115, &idx, 1)) { printf("hz: LINK_RATE_SET not taken\n"); return -1; }
   b5[0] = 0x21; memset(b5 + 1, ls, 4);
   if (dpcd_write(0x102, b5, 5)) return -1;
   int ok = 0;
   for (int t = 0; t < 5 && !ok; t++) {                           // clock recovery
      usleep(100);
      if (dpcd_read(0x202, st, 6)) return -1;
      printf("hz: CR %d: lanes %02x %02x align %02x adjust %02x %02x (laneset %02x)\n", t, st[0], st[1], st[2], st[4], st[5], ls);
      if ((st[0] & 0x11) == 0x11 && (st[1] & 0x11) == 0x11) { ok = 1; break; }
      unsigned sw = 0, pe = 0;
      for (int l = 0; l < 4; l++) { const unsigned a = (st[4 + l / 2] >> (4 * (l & 1))) & 0xf; if ((a & 3) > sw) sw = a & 3; if ((a >> 2) > pe) pe = a >> 2; }
      ls = (uint8_t)(sw | (pe << 3) | (sw == 3 ? 4 : 0) | (pe == 3 ? 0x20 : 0));
      for (int i = 0; i < 4; i++) vb_tx(0x0b, ls, 4, symclk, 0x14, 0xf6);
      uint8_t l4[4]; memset(l4, ls, 4); dpcd_write(0x103, l4, 4);
   }
   if (!ok) { printf("hz: clock recovery failed\n"); return -1; }
   wr_abs(0x55ee, 3); wr_abs(0x55de, 0); wr_abs(0x55ed, 0); wr_abs(0x55f3, 0x7fffff00);   // source TPS4
   for (int i = 0; i < 4; i++) vb_tx(0x0b, ls, 4, symclk, 0x14, 0xf6);
   b5[0] = 0x07; memset(b5 + 1, ls, 4);
   if (dpcd_write(0x102, b5, 5)) return -1;
   ok = 0;
   for (int t = 0; t < 6 && !ok; t++) {                           // equalization
      usleep(400);
      if (dpcd_read(0x202, st, 6)) return -1;
      printf("hz: EQ %d: lanes %02x %02x align %02x adjust %02x %02x (laneset %02x)\n", t, st[0], st[1], st[2], st[4], st[5], ls);
      if ((st[0] & 0x11) != 0x11 || (st[1] & 0x11) != 0x11) break;   // clock recovery lost
      if (st[0] == 0x77 && st[1] == 0x77 && (st[2] & 1)) { ok = 1; break; }
      unsigned sw = 0, pe = 0;
      for (int l = 0; l < 4; l++) { const unsigned a = (st[4 + l / 2] >> (4 * (l & 1))) & 0xf; if ((a & 3) > sw) sw = a & 3; if ((a >> 2) > pe) pe = a >> 2; }
      ls = (uint8_t)(sw | (pe << 3) | (sw == 3 ? 4 : 0) | (pe == 3 ? 0x20 : 0));
      for (int i = 0; i < 4; i++) vb_tx(0x0b, ls, 4, symclk, 0x14, 0xf6);
      uint8_t l4[4]; memset(l4, ls, 4); dpcd_write(0x103, l4, 4);
   }
   // end of training, as DC: the link's own framing and scrambling, the sink back to normal
   wr_abs(0x55e5, 1); wr_abs(0x55e9, 0x10102000); wr_abs(0x55f4, 0x0101ff10); wr_abs(0x55de, 0x10); wr_abs(0x55ed, 0); wr_abs(0x55f3, 0x7fffff00);
   const uint8_t z = 0; dpcd_write(0x102, &z, 1);
   if (!ok) printf("hz: equalization failed\n");
   return ok ? 0 : -1;
}
static int hz_otg_hz(void)   // the panel's refresh as OTG0's frame counter sees it over ~1 s, in mHz
{
   const uint32_t c0 = rd_abs(0x504d) & 0xffffff; const double t0 = now_ms();   // OTG0_OTG_STATUS_FRAME_COUNT
   usleep(1000000);
   const uint32_t c1 = rd_abs(0x504d) & 0xffffff; const double t1 = now_ms();
   return (int)(((c1 - c0) & 0xffffff) * 1e6 / (t1 - t0));
}
static int hz_switch(int hz)
{
   uint32_t base[HZ_NSAVED];
   uint8_t idx = 0, table[16];
   if (hz != 60 && hz != 120) { fprintf(stderr, "hz 60|120\n"); return 2; }
   if (dpcd_read(0x115, &idx, 1)) { printf("hz: STOP: the panel does not answer over AUX\n"); return 1; }
   const int now_hz = idx == 7 ? 120 : idx == 5 ? 60 : 0;
   printf("hz: the link runs at LINK_RATE_SET %u (%s), OTG %d mHz; going to %d Hz\n", idx, now_hz ? (now_hz == 60 ? "60 Hz" : "120 Hz") : "unknown",
          hz_otg_hz(), hz);
   if (!now_hz) { printf("hz: STOP: unknown link state\n"); return 1; }
   if (now_hz == hz) { printf("hz: already there\n"); return 0; }
   if (!hz_base(base, now_hz == 60)) { printf("hz: STOP: no 60 Hz values saved (%s) and the panel is not at 60 Hz\n", HZ_BASE); return 1; }
   if (dpcd_read(0x10, table, 16)) { printf("hz: STOP: cannot read the link-rate table\n"); return 1; }
   const uint8_t want = hz == 120 ? 7 : 5;
   const unsigned rate = table[want * 2] | (table[want * 2 + 1] << 8);    // 200 kHz units
   const uint32_t symclk = rate * 2;                                      // 10 kHz units: 40500 -> 81000 (810 MHz)
   printf("hz: LINK_RATE_SET %u = %u.%02u Gb/s, symclk %u0 kHz\n", want, rate / 5000, (rate / 50) % 100, symclk);
   if ((hz == 120 && rate != 40500) || (hz == 60 && rate != 21600)) { printf("hz: STOP: not the rate table of the 880M's panel\n"); return 1; }
   const double t0 = now_ms();
   const uint32_t dpg0 = rd_abs(0x4d14), vtg0 = rd_abs(0x39f7) | 0x80000000u;   // the firmware's DPG mode, VTG on
   if (hz == 120) { dcn_smu(8, 55); dcn_smu(4, 800); dcn_smu(6, 782); }
   // blank
   wr_abs(0x4d14, 0x41);                                          // DPG: solid black
   vb_lvtma(2); usleep(50000); vb_tx(0x02, 0, 4, 0, 0x14, 0xf4);  // backlight off
   wr_abs(0x55e2, 0x00010200); wr_abs(0x55e3, 0x3f000001);        // DP stream off
   wr_abs(0x5003, 0x00011200);                                    // OTG off, VTG with it
   wr_abs(0x39f7, vtg0 & ~0x80000000u);
   wr_abs(0x5044, 0x100); wr_abs(0x4f8f, 0x4);
   wr_abs(0x5601, 0); wr_abs(0x555b, 0x1000011c); wr_abs(0x5555, 0); wr_abs(0x5554, 0xc00); wr_abs(0x555b, 0x1c);
   wr_abs(0x557c, 0); wr_abs(0x160, 1); wr_abs(0x160, 0);         // DIG front end and back end off, symclk off
   { const uint8_t d3 = 2; dpcd_write(0x600, &d3, 1); }
   vb_tx(0x00, 0, 0, 0, 0x14, 0xf4); usleep(40000);               // TRANSMITTER DISABLE
   wr_abs(0x55ee, 0); wr_abs(0x55de, 0); wr_abs(0x55e5, 0);
   vb_lvtma(13); vb_tx(0x0d, 2, 4, 0, 0x14, 0xf6);                // panel power off
   const double toff = now_ms();
   // timing: pixel clock, DLG; OTG back on
   wr_abs(0x4f8f, 0x3); wr_abs(0x5044, 0x3);
   for (unsigned i = 0; i < HZ_NSAVED; i++) {
      uint32_t v = base[i];
      if (kHzSaved[i].reg == 0x0141) v = base[i] * (uint32_t)(hz / 60);                       // DP_DTO0_PHASE
      else if (kHzSaved[i].reg == 0x55e8) v = (uint32_t)((uint64_t)base[i] * (hz / 60) * 21600 / rate + 0);  // VID_M for this link rate
      else if (kHzSaved[i].refcyc) v = hz_scale(kHzSaved[i].refcyc, base[i], hz);
      if (kHzSaved[i].reg != 0x55e8 && kHzSaved[i].reg != 0x55e7) wr_abs(kHzSaved[i].reg, v);
   }
   wr_abs(0x39f7, vtg0);
   wr_abs(0x5003, 0x00001201);                                    // OTG on
   wr_abs(0x5553, 0x100); wr_abs(0x557b, 0); wr_abs(0x557b, 0x10); wr_abs(0x5003, 0x00011201);
   // link
   while (now_ms() - toff < 500) usleep(10000);                   // the panel stays off 500 ms, as DC keeps it
   vb_lvtma(12); vb_tx(0x0c, 2, 4, 0, 0x14, 0xf6);                // panel power on
   usleep(180000);
   wr_abs(0x557c, 0x100); wr_abs(0x5554, 0); wr_abs(0x53cd, 0); wr_abs(0x555b, 0x11c); wr_abs(0x5554, 0x10);
   wr_abs(0x5555, 1); wr_abs(0x555b, 0x11e); wr_abs(0x555b, 0x10011c); wr_abs(0x555b, 0x11d);
   wr_abs(0x55e1, 3); wr_abs(0x55f4, 0x0101ff10);
   vb_tx(0x01, 0, 4, symclk, 0x00, 0xf4);                         // TRANSMITTER ENABLE at the new rate
   { const uint8_t d0 = 1; dpcd_write(0x600, &d0, 1); usleep(1000); }
   { const uint8_t one = 1, zero = 0, ss = 0x10, lc = 0x84; dpcd_write(0x10a, &one, 1); dpcd_write(0x102, &zero, 1);
     dpcd_write(0x108, &one, 1); dpcd_write(0x107, &ss, 1); dpcd_write(0x101, &lc, 1); dpcd_write(0x100, &zero, 1); }
   const int trained = hz_train(want, symclk);
   // stream
   uint32_t vidm = (uint32_t)((uint64_t)base[16] * (hz / 60) * 21600 / rate);   // M scales with pixel clock / link rate
   wr_abs(0x557b, 0x810); wr_abs(0x160, 0x11); wr_abs(0x557c, 0x100); wr_abs(0x5554, 0xc10); wr_abs(0x53cd, 0);
   wr_abs(0x555b, 0x11d); wr_abs(0x5554, 0xc10); wr_abs(0x5555, 1); wr_abs(0x555b, 0x11f); wr_abs(0x555b, 0x10011d); wr_abs(0x555b, 0x11d);
   wr_abs(0x5601, 0); wr_abs(0x55df, 0x42000000);                 // no secondary data packets: the firmware sends none wr_abs(0x55e6, 0x400); wr_abs(0x55e7, base[17]); wr_abs(0x55e8, vidm);
   wr_abs(0x55e6, 0x500); wr_abs(0x55e2, 0x00100200); wr_abs(0x55e3, 0x3f000001); wr_abs(0x55e3, 0x3f000000); wr_abs(0x55e2, 0x00100201);
   wr_abs(0x555b, 0x11d); wr_abs(0x555b, 0x11f); wr_abs(0x555b, 0x10011d); wr_abs(0x555b, 0x11d);
   vb_lvtma(3); vb_tx(0x03, 0, 4, 0, 0x14, 0xf4);                 // backlight on
   wr_abs(0x4d14, dpg0 & ~1u);                                    // DPG off: the picture
   usleep(100000);
   uint8_t sst[3] = {0}; dpcd_read(0x202, sst, 3);
   const uint32_t uf = (rd_abs(0x3ab3) >> 28) & 7;
   printf("hz: done in %.0f ms: link %s (lanes %02x %02x align %02x), VID_M %08x, underflow %u, OTG now %d mHz\n", now_ms() - t0,
          trained ? "NOT trained" : "trained", sst[0], sst[1], sst[2], vidm, uf, hz_otg_hz());
   return trained ? 3 : 0;
}

// mesboot dpcd r <addr> <len> | dpcd w <addr> <hex bytes> | dmub <hex bytes of one command> | dmubstate
static int link_cmd(int argc, char **argv)
{
   if (!strcmp(argv[1], "dmubstate")) { dmub_state("now"); return 0; }
   if (!strcmp(argv[1], "hz") && argc > 2) return hz_switch(atoi(argv[2]));
   if (!strcmp(argv[1], "dmub") && argc > 2) {
      uint8_t cmd[64] = {0};
      const int n = hexbytes(argv[2], cmd, 64);
      dmub_state("before");
      const int r = n >= 4 ? dmub_send(cmd, 500) : -9;
      printf("dmub: %d-byte command type %u sub %u -> %s\n", n, cmd[0], cmd[1], r == 0 ? "executed (RPTR passed it)" :
             r == -1 ? "NOT sent: DMCUB busy with an earlier one" : r == -2 ? "NOT TAKEN within 500 ms" : "bad command");
      dmub_state("after");
      return r ? 1 : 0;
   }
   if (!strcmp(argv[1], "dpcd") && argc > 3) {
      const uint32_t addr = (uint32_t)strtoul(argv[3], NULL, 0);
      if (argv[2][0] == 'r') {
         const unsigned len = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 1;
         uint8_t b[16] = {0};
         if (len < 1 || len > 16) { fprintf(stderr, "len 1..16\n"); return 2; }
         const int r = dpcd_read(addr, b, len);
         printf("dpcd 0x%05x:", addr);
         if (r == 0) { for (unsigned i = 0; i < len; i++) printf(" %02x", b[i]); printf("\n"); }
         else printf(" %s (%d)\n", r == 0x10 ? "NACK" : r == 0x20 ? "DEFER" : "no reply", r);
         return r ? 1 : 0;
      }
      if (argv[2][0] == 'w' && argc > 4) {
         uint8_t b[16]; const int n = hexbytes(argv[4], b, 16);
         const int r = n > 0 ? dpcd_write(addr, b, (unsigned)n) : -9;
         printf("dpcd 0x%05x <- %d bytes: %s (%d)\n", addr, n, r == 0 ? "ACK" : r == 0x10 ? "NACK" : "failed", r);
         return r ? 1 : 0;
      }
   }
   fprintf(stderr, "mesboot dpcd r <addr> [len] | dpcd w <addr> <hex> | dmub <hex> | dmubstate\n");
   return 2;
}

// NMXAccel starts only after `claim`, so once it runs, the bring-up of this boot is done (phxboot, or a test script).
// A second gart/mes/queue setup under it rewrites the page tables, rings and MES state its clients are using: on a
// boot with the phxboot LaunchDaemon, an old try script did exactly that and the next Metal client hung (2026-10-25).
static int accel_running(void)
{
   io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
   if (!s) return 0;
   IOObjectRelease(s);
   return 1;
}

int main(int argc, char **argv)
{
   if (open_kext()) return 1;
   if (argc < 2) { fprintf(stderr, "mesboot gart|mes|kiqsched|gpustat|claim|gfx|all ...\n"); return 2; }
   if (strcmp(argv[1], "gpustat") && strcmp(argv[1], "dump") && strcmp(argv[1], "claim") && strcmp(argv[1], "dcnflip") &&
       strcmp(argv[1], "sdmatest") && strcmp(argv[1], "dpcd") && strcmp(argv[1], "dmub") && strcmp(argv[1], "dmubstate") && strcmp(argv[1], "hz") &&
       accel_running() &&
       !(getenv("MESBOOT_FORCE") && *getenv("MESBOOT_FORCE") == '1')) {
      fprintf(stderr, "mesboot: STOP: the Metal accelerator (NMXAccel) already runs on the GPU, so this boot's bring-up "
                      "is done (phxboot or an earlier run); '%s' would rewrite the page tables and queues it uses. "
                      "Reboot first (MESBOOT_FORCE=1 overrides)\n", argv[1]);
      return 3;
   }
   if (chip->id != RDNA3_CHIP_PHOENIX) {
      uint32_t off = rd(SEG0(0x15a7)) & 0xffffff;    // GCMC_VM_FB_OFFSET: where the BIOS put the carve-out
      if (!off) { fprintf(stderr, "mesboot: GCMC_VM_FB_OFFSET reads 0, GPU not up?\n"); return 1; }
      fb_pa = (uint64_t)off << 24;
   }
   int all = !strcmp(argv[1], "all");
   if (getenv("MESBOOT_MC") && *getenv("MESBOOT_MC") == '1') use_mc = 1;
   if (!strcmp(argv[1], "db") && argc >= 4) { printf("db +0x%llx <- %llu kr=0x%x\n", strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0), db(strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0))); return 0; }
   if (!strcmp(argv[1], "dump")) mesdump();
   if (all || !strcmp(argv[1], "gart")) gart();
   if (all || !strcmp(argv[1], "mes")) mes_start();
   if (!strcmp(argv[1], "kiqdb")) { use_db = 1; kiq_test(); }
   if (!strcmp(argv[1], "kiqsched")) { kiq_init_only(); sched_test(); }
   if (!strcmp(argv[1], "claim")) {   // GPU is up: let PhxAccel (the Metal accelerator) start on it
      uint64_t in[1] = {1};
      printf("claim -> kr 0x%x\n", IOConnectCallScalarMethod(conn, 113, in, 1, NULL, NULL));
   }
   if (!strcmp(argv[1], "gfx")) {   // MES already running (after kiqsched): continue its ring where the last run left it
      sch_wptr = vr(SCH_WB + 16); sch_seq = vr(SCH_WB + 32);
      printf("sched ring: wptr=%u seq=%llu\n", sch_wptr, (unsigned long long)sch_seq);
      gfx_via_mes();
   }
   if (!strcmp(argv[1], "gpustat")) gpustat();
   if (!strcmp(argv[1], "dcnflip")) return dcnflip(argc > 2 ? atoi(argv[2]) : 4);
   if (!strcmp(argv[1], "sdmasys")) sdma_sys_test();
   if (!strcmp(argv[1], "sdmatest")) return sdmatest(argc > 2 ? atoi(argv[2]) : 10);
   if (!strcmp(argv[1], "dpcd") || !strcmp(argv[1], "dmub") || !strcmp(argv[1], "dmubstate") || !strcmp(argv[1], "hz")) return link_cmd(argc, argv);
   if (!strcmp(argv[1], "kiqreg")) kiqreg_test();
   if (!strcmp(argv[1], "mec")) mec_test();
   if (!strcmp(argv[1], "sched")) sched_test();
   if (!strcmp(argv[1], "viakiq")) { use_db = 1; kiq_test(); skip_hqd = 1; sched_test(); }
   if (all || !strcmp(argv[1], "kiq")) kiq_test();
   return 0;
}
