// vcnjob: VCN jobs through PhxBringup's client ABI, the way RADV sends them (build 42+, after `mesboot vcn up` and
// `mesboot vcn adopt`). Each job is one IB of VCN 4's unified queue holding a single "write memory" command (what RADV
// emits for vkCmdSetEvent on a video queue, Mesa ac_emit_video_write_memory): the engine writes a value into the
// client's buffer through its own GPUVM context (MMHUB), the kext's fence says the job is done, the CPU checks the value.
// Build: clang -O2 -I../../src/phxbringup -I../../src/amd -o vcnjob vcnjob.c -framework IOKit
//   vcnjob [jobs]     (default 8)        exit 0 = every value arrived
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PhxNativeABI.h"
#include "vcn_v4_0_5.h"

static io_connect_t conn;
static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

int main(int argc, char **argv)
{
   const int jobs = argc > 1 ? atoi(argv[1]) : 8;
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!svc || IOServiceOpen(svc, mach_task_self(), PHXN_UC_TYPE, &conn)) { printf("FAIL: cannot open PhxBringup\n"); return 1; }
   uint64_t hin[2] = {PHXN_ABI_VERSION, 0}, hout[4]; uint32_t hn = 4;
   if (IOConnectCallScalarMethod(conn, PHXN_SEL_HELLO, hin, 2, hout, &hn)) { printf("FAIL: HELLO\n"); return 1; }
   struct phxn_info inf; size_t isz = sizeof inf;
   if (IOConnectCallStructMethod(conn, PHXN_SEL_QUERYINFO, NULL, 0, &inf, &isz)) { printf("FAIL: QUERYINFO\n"); return 1; }
   printf("vcnjob: PhxBringup build %u, flags 0x%x, VCN %s, firmware 0x%08x\n", inf.kext_build, inf.flags,
          inf.flags & PHXN_INFO_VCN ? "adopted" : "NOT adopted", inf.reserved[0]);
   if (!(inf.flags & PHXN_INFO_VCN)) { printf("FAIL: no VCN (run mesboot vcn up + adopt first)\n"); return 1; }

   struct phxn_ctx cin = {PHX_AMDGPU_CTX_OP_ALLOC_CTX, 0, 0, 0}, cout; size_t csz = sizeof cout;
   if (IOConnectCallStructMethod(conn, PHXN_SEL_CTX, &cin, sizeof cin, &cout, &csz)) { printf("FAIL: CTX\n"); return 1; }
   const uint32_t ctx_id = cout.op;   // drm_amdgpu_ctx_out: alloc.ctx_id is the first word, where `op` sits
   struct phxn_gem_create_in bin = {65536, 4096, PHX_AMDGPU_GEM_DOMAIN_GTT, 0};
   uint64_t bout[4] = {0}; uint32_t bn = 4;
   if (IOConnectCallMethod(conn, PHXN_SEL_BOCREATE, NULL, 0, &bin, sizeof bin, bout, &bn, NULL, NULL)) { printf("FAIL: BOCREATE\n"); return 1; }
   const uint32_t bo = (uint32_t)bout[0];
   mach_vm_address_t cpu = 0; mach_vm_size_t len = 0;
   if (IOConnectMapMemory64(conn, bo, mach_task_self(), &cpu, &len, kIOMapAnywhere)) { printf("FAIL: map\n"); return 1; }
   const uint64_t va = 0x200000;
   struct phxn_gem_va gv; memset(&gv, 0, sizeof gv);
   gv.handle = bo; gv.operation = PHX_AMDGPU_VA_OP_MAP; gv.flags = (1u << 1) | (1u << 2) | PHX_AMDGPU_VM_PAGE_EXECUTABLE;
   gv.va_address = va; gv.map_size = 65536;
   if (IOConnectCallStructMethod(conn, PHXN_SEL_GEMVA, &gv, sizeof gv, NULL, NULL)) { printf("FAIL: GEMVA\n"); return 1; }
   printf("vcnjob: context %u, buffer %u (64 KiB GTT) at GPU VA 0x%llx\n", ctx_id, bo, (unsigned long long)va);

   volatile uint32_t *m = (volatile uint32_t *)cpu;
   int ok = 0;
   for (int j = 0; j < jobs; j++) {
      const uint32_t value = 0x5600CAFEu + (uint32_t)j * 0x01010101u;
      const uint32_t slot = 0x1000 / 4 + (uint32_t)j;           // target dword for this job
      m[slot] = 0;
      const uint64_t dst = va + 4ull * slot;
      vcn45_write_memory_ib((uint32_t *)cpu, dst, value);     // the IB at the buffer's start
      __sync_synchronize();
      struct { struct phxn_cs_in h; struct phxn_cs_ib ib; } cs;
      memset(&cs, 0, sizeof cs);
      cs.h.abi = PHXN_ABI_VERSION; cs.h.ctx_id = ctx_id; cs.h.num_ibs = 1;
      cs.ib.va_start = va; cs.ib.ib_bytes = VCN45_WRITE_MEMORY_DW * 4; cs.ib.ip_type = PHX_AMDGPU_HW_IP_VCN_ENC;
      uint64_t seq = 0; uint32_t sn = 1;
      const double t0 = now_ms();
      kern_return_t kr = IOConnectCallMethod(conn, PHXN_SEL_SUBMIT, NULL, 0, &cs, sizeof cs, &seq, &sn, NULL, NULL);
      if (kr) { printf("vcnjob: job %d: SUBMIT -> 0x%x\n", j, kr); break; }
      uint64_t win[3] = {seq, 2000000000ull, 0}, wout[3] = {0}; uint32_t wn = 3;
      kr = IOConnectCallScalarMethod(conn, PHXN_SEL_WAITSEQ, win, 3, wout, &wn);
      const double ms = now_ms() - t0;
      const uint32_t got = m[slot];
      const int good = !kr && !wout[0] && got == value;
      ok += good;
      printf("vcnjob: job %d seq %llu: %s in %.2f ms, value 0x%08x %s 0x%08x\n", j, (unsigned long long)seq,
             kr ? "WAIT FAILED" : wout[0] ? "NOT DONE (2 s)" : "done", ms, got, good ? "==" : "!=", value);
      if (!good) break;
   }
   printf("vcnjob: %s (%d of %d)\n", ok == jobs ? "PASS" : "FAIL", ok, jobs);
   IOServiceClose(conn);
   return ok == jobs ? 0 : 1;
}
