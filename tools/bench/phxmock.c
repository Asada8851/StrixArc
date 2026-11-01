/* phxmock: a host (Linux) model of PhxBringup's user client (IOAccelPhxNativeClient, build 43), behind the IOKit calls
 * of tools/bench/include/IOKit/IOKitLib.h, so the clients (vcnjob, later the Mesa Darwin backend) run on the bench
 * before a hardware round. Every handler mirrors the kext function named in its comment: the same shape checks, the
 * same validation, the same output layout (the context id in the first word of the CTX reply, ...). The "GPU" runs a
 * submission at once: VCN IBs are parsed and their common-engine WRITEMEMORY packages executed through the client's
 * mappings; anything else in a VCN IB is reported. Keep it in step with PhxBringup.cpp when that changes.
 * PHXMOCK_NOVCN=1: VCN not adopted. PHXMOCK_TRACE=1: every call printed. */
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "PhxNativeABI.h"
#include "vcn_v4_0_5.h"

#define MAX_MAPS 512
static const uint64_t kVaMask = (1ull << 48) - 1;
static const uint64_t kVisTotal = 256ull << 20, kHiTotal = 1536ull << 20, kGttCap = 512ull << 20, kGttMaxBo = 256ull << 20;

static struct {
   int open, hello, vcn;
   int ctx[PHXN_MAX_CTX + 1];
   struct { uint8_t *mem; uint64_t size; uint32_t domain, bits; } bo[PHXN_MAX_BOS];
   struct { int used; uint32_t handle; uint64_t va, size, off; uint32_t flags; } map[MAX_MAPS];
   uint64_t visUsed, hiUsed, gttUsed, emitted;
   unsigned vcnWrites, vcnUnknown;
} m;
static int trace;
/* return a refusal, saying which check made it (PHXMOCK_TRACE) */
#define REFUSE(rc, why) do { if (trace) printf("phxmock: %s: refused (%s)\n", __func__, why); return (rc); } while (0)

mach_port_t mach_task_self(void) { return 1; }
CFDictionaryRef IOServiceMatching(const char *name) { return name; }
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching)
{
   (void)main;
   return matching && !strcmp((const char *)matching, "PhxBringup") ? 0x1234 : 0;
}
kern_return_t IOServiceOpen(io_service_t svc, task_t task, uint32_t type, io_connect_t *conn)
{
   (void)task;
   if (svc != 0x1234 || type != PHXN_UC_TYPE) return kIOReturnBadArgument;
   memset(&m, 0, sizeof m);
   m.open = 1; m.vcn = !getenv("PHXMOCK_NOVCN"); trace = getenv("PHXMOCK_TRACE") != NULL;
   *conn = 0x5678;
   return kIOReturnSuccess;
}
kern_return_t IOServiceClose(io_connect_t c)
{
   (void)c;
   for (unsigned h = 1; h < PHXN_MAX_BOS; h++) free(m.bo[h].mem);
   if (trace || m.vcnUnknown) printf("phxmock: closed; VCN wrote %u values, %u packages it does not know\n", m.vcnWrites, m.vcnUnknown);
   m.open = 0;
   return kIOReturnSuccess;
}
kern_return_t IOObjectRelease(io_object_t o) { (void)o; return 0; }

/* the kext's shape(): exact scalar counts and struct sizes */
struct args { const uint64_t *in; uint32_t inCnt; const void *inS; size_t inSCnt; uint64_t *out; uint32_t outCnt; void *outS; size_t outSCnt; };
static int shape(const struct args *a, uint32_t sin, uint32_t sout, size_t stin, size_t stout)
{ return a->inCnt == sin && a->outCnt == sout && a->inSCnt == stin && a->outSCnt == stout; }

static uint8_t *va_ptr(uint64_t va, uint64_t bytes)   /* a client VA -> the BO memory behind it (one mapping) */
{
   va &= kVaMask;
   for (int i = 0; i < MAX_MAPS; i++)
      if (m.map[i].used && va >= m.map[i].va && va + bytes <= m.map[i].va + m.map[i].size)
         return m.bo[m.map[i].handle].mem + m.map[i].off + (va - m.map[i].va);
   return NULL;
}
static int va_canonical(uint64_t va) { uint64_t top = va >> 47; return top == 0 || top == 0x1FFFF; }
/* IOAccelPhxNativeClient::ibCovered: only EXECUTABLE mappings may hold IBs */
static int ib_covered(uint64_t va, uint64_t bytes)
{
   uint64_t cur = va & kVaMask; const uint64_t end = cur + bytes;
   while (cur < end) {
      int ok = 0;
      for (int i = 0; i < MAX_MAPS; i++)
         if (m.map[i].used && (m.map[i].flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= m.map[i].va && cur < m.map[i].va + m.map[i].size) {
            cur = m.map[i].va + m.map[i].size < end ? m.map[i].va + m.map[i].size : end; ok = 1; break;
         }
      if (!ok) return 0;
   }
   return 1;
}

/* the VCN firmware's view of one unified-queue IB: engine info blocks, each holding packages (Mesa ac_vcn.h) */
static void vcn_run_ib(uint64_t va, uint32_t dw)
{
   const uint32_t *ib = (const uint32_t *)va_ptr(va, 4ull * dw);
   if (!ib) { printf("phxmock: VCN IB at 0x%llx not mapped\n", (unsigned long long)va); m.vcnUnknown++; return; }
   uint32_t i = 0;
   while (i + 4 <= dw) {
      if (ib[i] != VCN45_ENGINE_INFO_SIZE || ib[i + 1] != VCN45_ENGINE_INFO) {
         printf("phxmock: VCN IB word %u: 0x%08x 0x%08x is no engine info\n", i, ib[i], ib[i + 1]); m.vcnUnknown++; return;
      }
      const uint32_t type = ib[i + 2], bytes = ib[i + 3];
      if (bytes < 16 || bytes % 4 || i + 3 + bytes / 4 - 3 > dw) { printf("phxmock: VCN engine block of %u bytes runs past the IB\n", bytes); m.vcnUnknown++; return; }
      const uint32_t end = i + 3 + bytes / 4 - 3;   /* the size counts from the size word on, + 3 (ac_vcn_sq_tail) */
      i += 4;
      while (i + 2 <= end) {
         const uint32_t psize = ib[i], ptype = ib[i + 1];
         if (psize < 8 || psize % 4 || i + psize / 4 > end) { printf("phxmock: VCN package size %u\n", psize); m.vcnUnknown++; return; }
         if (type == VCN45_ENGINE_TYPE_COMMON && ptype == VCN45_OP_WRITEMEMORY && psize == 20) {
            const uint64_t dst = ib[i + 2] | (uint64_t)ib[i + 3] << 32;
            uint32_t *p = (uint32_t *)va_ptr(dst, 4);
            if (p) { *p = ib[i + 4]; m.vcnWrites++; }
            else { printf("phxmock: WRITEMEMORY to unmapped 0x%llx (an MMHUB fault on the hardware)\n", (unsigned long long)dst); m.vcnUnknown++; }
         } else {
            if (trace) printf("phxmock: VCN engine %u package 0x%08x (%u bytes) not modelled\n", type, ptype, psize);
            m.vcnUnknown++;
         }
         i += psize / 4;
      }
      i = end;
   }
}

static IOReturn sel_hello(struct args *a)            /* doHello */
{
   if (!shape(a, 2, 4, 0, 0)) return kIOReturnBadArgument;
   if (a->in[1] != 0 && a->in[1] != PHXN_HELLO_F_MINOR) return kIOReturnBadArgument;
   if (a->in[0] != PHXN_ABI_VERSION) return kIOReturnUnsupported;
   m.hello = 1;
   a->out[0] = PHXN_ABI_VERSION | (a->in[1] ? (uint64_t)PHXN_ABI_MINOR << 16 : 0);
   a->out[1] = 43; a->out[2] = PHXN_MAX_IBS; a->out[3] = PHXN_VMID;
   return kIOReturnSuccess;
}
static IOReturn sel_queryinfo(struct args *a)        /* doQueryInfo */
{
   if (!shape(a, 0, 0, 0, sizeof(struct phxn_info))) return kIOReturnBadArgument;
   struct phxn_info inf; memset(&inf, 0, sizeof inf);
   inf.abi_version = PHXN_ABI_VERSION; inf.kext_build = 43; inf.reserved[2] = PHXN_ABI_MINOR;
   inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT;
   if (m.vcn) { inf.flags |= PHXN_INFO_VCN; inf.reserved[0] = 0x09118022; }
   inf.vmid = PHXN_VMID; inf.gb_addr_config = 0x242; inf.gc_version = 0x0b0500; inf.ring_size_dw = 4096; inf.max_ibs = PHXN_MAX_IBS;
   inf.vram_vis_total = kVisTotal; inf.vram_vis_free = kVisTotal - m.visUsed; inf.vram_hi_total = kHiTotal; inf.vram_hi_free = kHiTotal - m.hiUsed;
   inf.gtt_cap = kGttCap; inf.gtt_used = m.gttUsed; inf.gtt_max_bo = kGttMaxBo;
   inf.seq_emitted = inf.seq_retired = m.emitted;
   inf.va_low_first = 0x10000; inf.va_low_last = 0x7FFFFFFFFFFFull; inf.va_high_first = 0xFFFF800000000000ull; inf.va_high_last = ~0ull;
   inf.max_bos = PHXN_MAX_BOS; inf.fence_slots = PHXN_FENCE_SLOTS;
   memcpy(a->outS, &inf, sizeof inf);
   return kIOReturnSuccess;
}
static IOReturn sel_ctx(struct args *a)              /* doCtx: the new id goes out in the FIRST word (out.op) */
{
   if (!shape(a, 0, 0, sizeof(struct phxn_ctx), sizeof(struct phxn_ctx))) return kIOReturnBadArgument;
   struct phxn_ctx in, out; memcpy(&in, a->inS, sizeof in); memset(&out, 0, sizeof out);
   if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
      uint32_t id = 1;
      while (id <= PHXN_MAX_CTX && m.ctx[id]) id++;
      if (id > PHXN_MAX_CTX) return kIOReturnNoResources;
      m.ctx[id] = 1; out.op = id;
   } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
      if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !m.ctx[in.ctx_id]) return kIOReturnNotFound;
      m.ctx[in.ctx_id] = 0;
   } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) return kIOReturnBadArgument;
   memcpy(a->outS, &out, sizeof out);
   return kIOReturnSuccess;
}
static IOReturn sel_bocreate(struct args *a)         /* doBoCreate */
{
   if (!shape(a, 0, 4, sizeof(struct phxn_gem_create_in), 0)) return kIOReturnBadArgument;
   const struct phxn_gem_create_in *in = a->inS;
   uint64_t size = in->bo_size;
   if (!size) return kIOReturnBadArgument;
   size = (size + 4095) & ~4095ull;
   const uint64_t align = in->alignment;
   if (align && ((align & (align - 1)) || align > (2ull << 20))) return kIOReturnBadArgument;
   const uint64_t d = in->domains, f = in->domain_flags;
   if (d == 0 || (d & ~(uint64_t)(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM))) return kIOReturnUnsupported;
   if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE)) return kIOReturnUnsupported;
   uint32_t domain, bits; uint64_t *pool;
   if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
      if (m.visUsed + size <= kVisTotal) { domain = 4; bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED; pool = &m.visUsed; }
      else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) && m.hiUsed + size <= kHiTotal)
         { domain = 4; bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED; pool = &m.hiUsed; }
      else return kIOReturnNoMemory;
   } else {
      if (size > kGttMaxBo || m.gttUsed + size > kGttCap) return kIOReturnNoMemory;
      domain = 2; bits = PHXN_PLACED_ZEROED; pool = &m.gttUsed;
   }
   uint32_t h = 1;
   while (h < PHXN_MAX_BOS && m.bo[h].mem) h++;
   if (h >= PHXN_MAX_BOS) return kIOReturnNoResources;
   m.bo[h].mem = calloc(1, size);
   if (!m.bo[h].mem) return kIOReturnNoMemory;
   m.bo[h].size = size; m.bo[h].domain = domain; m.bo[h].bits = bits; *pool += size;
   a->out[0] = h; a->out[1] = size; a->out[2] = domain; a->out[3] = bits;
   return kIOReturnSuccess;
}
static IOReturn sel_bofree(struct args *a)           /* doBoFree: its mappings go with it */
{
   if (!shape(a, 1, 0, 0, 0)) return kIOReturnBadArgument;
   const uint32_t h = (uint32_t)a->in[0];
   if (h == 0 || h >= PHXN_MAX_BOS || !m.bo[h].mem) return kIOReturnNotFound;
   for (int i = 0; i < MAX_MAPS; i++)
      if (m.map[i].used && m.map[i].handle == h) m.map[i].used = 0;
   if (m.bo[h].domain == 2) m.gttUsed -= m.bo[h].size;
   else if (m.bo[h].bits & PHXN_PLACED_HI_POOL) m.hiUsed -= m.bo[h].size;
   else m.visUsed -= m.bo[h].size;
   free(m.bo[h].mem); memset(&m.bo[h], 0, sizeof m.bo[h]);
   return kIOReturnSuccess;
}
static IOReturn sel_gemva(struct args *a)            /* doGemVa */
{
   if (!shape(a, 0, 0, sizeof(struct phxn_gem_va), 0)) return kIOReturnBadArgument;
   const struct phxn_gem_va *v = a->inS;
   if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles || v->input_fence_syncobj_handles) return kIOReturnBadArgument;
   if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT)) return kIOReturnUnsupported;
   if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE) return kIOReturnUnsupported;
   if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP) return kIOReturnBadArgument;
   if (v->handle >= PHXN_MAX_BOS || !m.bo[v->handle].mem) return kIOReturnNotFound;
   if (!va_canonical(v->va_address)) return kIOReturnBadArgument;
   const uint64_t va = v->va_address & kVaMask;
   if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0) return kIOReturnBadArgument;
   if (va < 0x10000 || va + v->map_size > (1ull << 48)) return kIOReturnBadArgument;
   if (v->offset_in_bo + v->map_size > m.bo[v->handle].size) return kIOReturnBadArgument;
   if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
      int fr = -1;
      for (int i = 0; i < MAX_MAPS; i++) {
         if (!m.map[i].used) { if (fr < 0) fr = i; continue; }
         if (va < m.map[i].va + m.map[i].size && m.map[i].va < va + v->map_size) return kIOReturnBadArgument;
      }
      if (fr < 0) return kIOReturnNoMemory;
      m.map[fr].used = 1; m.map[fr].handle = v->handle; m.map[fr].va = va; m.map[fr].size = v->map_size;
      m.map[fr].off = v->offset_in_bo; m.map[fr].flags = v->flags;
      return kIOReturnSuccess;
   }
   for (int i = 0; i < MAX_MAPS; i++)
      if (m.map[i].used && m.map[i].handle == v->handle && m.map[i].va == va && m.map[i].size == v->map_size) { m.map[i].used = 0; return kIOReturnSuccess; }
   return kIOReturnBadArgument;
}
static IOReturn sel_submit(struct args *a)           /* doSubmit (build 42 rules: VCN IBs only when adopted, one engine) */
{
   if (a->inCnt != 0 || a->outCnt != 1 || a->outSCnt != 0 || a->inSCnt < 64 || a->inSCnt > 32 + 32 * PHXN_MAX_IBS) REFUSE(kIOReturnBadArgument, "a->inCnt != 0 || a->outCnt != 1 || a->outSCnt != 0 || a->inSCnt < 64 || a->inSCnt > 32 + 3");
   const uint8_t *in = a->inS;
   struct phxn_cs_in h; memcpy(&h, in, sizeof h);
   if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS || a->inSCnt != sizeof h + h.num_ibs * sizeof(struct phxn_cs_ib) ||
       h.reserved != 0 || (h.flags & ~PHXN_CS_HAS_FENCE)) REFUSE(kIOReturnBadArgument, "h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS || a->inSCnt != siz");
   if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !m.ctx[h.ctx_id]) REFUSE(kIOReturnNotFound, "h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !m.ctx[h.ctx_id]");
   uint32_t ip0 = 0;
   struct phxn_cs_ib ib[PHXN_MAX_IBS];
   for (uint32_t i = 0; i < h.num_ibs; i++) {
      memcpy(&ib[i], in + sizeof h + i * sizeof ib[0], sizeof ib[0]);
      const int vcnIb = ib[i].ip_type == PHX_AMDGPU_HW_IP_VCN_ENC && m.vcn;
      if (trace) printf("phxmock: ib %u: pad %u flags 0x%x va 0x%llx bytes %u ip %u inst %u ring %u (VCN %s)\n", i, ib[i]._pad,
                        ib[i].flags, (unsigned long long)ib[i].va_start, ib[i].ib_bytes, ib[i].ip_type, ib[i].ip_instance,
                        ib[i].ring, m.vcn ? "on" : "off");
      if (ib[i]._pad || (ib[i].ip_type != PHX_AMDGPU_HW_IP_GFX && ib[i].ip_type != PHX_AMDGPU_HW_IP_COMPUTE && !vcnIb) ||
          ib[i].ip_instance != 0 || ib[i].ring != 0) REFUSE(kIOReturnBadArgument, "ib[i]._pad || (ib[i].ip_type != PHX_AMDGPU_HW_IP_GFX && ib[i].ip_type != PHX_AMDGPU_HW_IP_");
      if (i == 0) ip0 = ib[i].ip_type;
      else if ((ib[i].ip_type == PHX_AMDGPU_HW_IP_VCN_ENC) != (ip0 == PHX_AMDGPU_HW_IP_VCN_ENC)) REFUSE(kIOReturnBadArgument, "(ib[i].ip_type == PHX_AMDGPU_HW_IP_VCN_ENC) != (ip0 == PHX_AMDGPU_HW_IP_VCN_ENC)");
      if (ib[i].ib_bytes == 0 || (ib[i].ib_bytes & 3) || ib[i].ib_bytes > 0xFFFFFu * 4) REFUSE(kIOReturnBadArgument, "ib[i].ib_bytes == 0 || (ib[i].ib_bytes & 3) || ib[i].ib_bytes > 0xFFFFFu * 4");
      if (!va_canonical(ib[i].va_start) || (ib[i].va_start & 3)) REFUSE(kIOReturnBadArgument, "!va_canonical(ib[i].va_start) || (ib[i].va_start & 3)");
      if (ib[i].flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE | PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
         return kIOReturnUnsupported;
      if (ib[i].flags & ~(uint32_t)(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT | PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
         return kIOReturnBadArgument;
      if (!ib_covered(ib[i].va_start, ib[i].ib_bytes)) REFUSE(kIOReturnBadArgument, "!ib_covered(ib[i].va_start, ib[i].ib_bytes)");
   }
   if (h.flags & PHXN_CS_HAS_FENCE) {
      if (h.fence_handle >= PHXN_MAX_BOS || !m.bo[h.fence_handle].mem) REFUSE(kIOReturnNotFound, "h.fence_handle >= PHXN_MAX_BOS || !m.bo[h.fence_handle].mem");
      if ((h.fence_offset & 7) || (uint64_t)h.fence_offset + 8 > m.bo[h.fence_handle].size) REFUSE(kIOReturnBadArgument, "(h.fence_offset & 7) || (uint64_t)h.fence_offset + 8 > m.bo[h.fence_handle].size");
   } else if (h.fence_handle || h.fence_offset) REFUSE(kIOReturnBadArgument, "h.fence_handle || h.fence_offset");
   /* the GPU: VCN jobs are executed here, CP jobs only retire */
   if (ip0 == PHX_AMDGPU_HW_IP_VCN_ENC)
      for (uint32_t i = 0; i < h.num_ibs; i++) vcn_run_ib(ib[i].va_start & kVaMask, ib[i].ib_bytes / 4);
   const uint64_t seq = ++m.emitted;
   if (h.flags & PHXN_CS_HAS_FENCE) memcpy(m.bo[h.fence_handle].mem + h.fence_offset, &seq, 8);
   a->out[0] = seq;
   return kIOReturnSuccess;
}
static IOReturn sel_waitseq(struct args *a)          /* doWaitSeq (everything submitted has run) */
{
   if (!m.hello) return kIOReturnNotReady;
   if (!shape(a, 3, 3, 0, 0)) return kIOReturnBadArgument;
   uint64_t t = a->in[0];
   if (t == PHXN_SEQ_LAST) t = m.emitted;
   if (t > m.emitted) return kIOReturnBadArgument;
   a->out[0] = 0; a->out[1] = m.emitted; a->out[2] = m.emitted;
   return kIOReturnSuccess;
}

kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, const void *inS, size_t inSCnt,
                                  uint64_t *out, uint32_t *outCnt, void *outS, size_t *outSCnt)
{
   if (c != 0x5678 || !m.open) return kIOReturnBadArgument;
   struct args a = {in, inCnt, inS, inSCnt, out, outCnt ? *outCnt : 0, outS, outSCnt ? *outSCnt : 0};
   IOReturn r;
   /* the kext's user client runs one call at a time (its work loop gate) */
   static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&gate);
   if (sel == PHXN_SEL_WAITSEQ) r = sel_waitseq(&a);
   else if (sel != PHXN_SEL_HELLO && !m.hello) r = kIOReturnNotReady;
   else switch (sel) {
   case PHXN_SEL_HELLO: r = sel_hello(&a); break;
   case PHXN_SEL_QUERYINFO: r = sel_queryinfo(&a); break;
   case PHXN_SEL_BOCREATE: r = sel_bocreate(&a); break;
   case PHXN_SEL_BOFREE: r = sel_bofree(&a); break;
   case PHXN_SEL_GEMVA: r = sel_gemva(&a); break;
   case PHXN_SEL_CTX: r = sel_ctx(&a); break;
   case PHXN_SEL_SUBMIT: r = sel_submit(&a); break;
   default: r = kIOReturnBadArgument; printf("phxmock: selector %u not modelled\n", sel); break;
   }
   if (trace) printf("phxmock: selector %u -> 0x%x\n", sel, r);
   pthread_mutex_unlock(&gate);
   /* PHXMOCK_FREE_DELAY_US: the caller is preempted right after a BO free returns, as a kernel can do - the moment
    * another thread's allocation may get the same handle back */
   static int free_delay = -1;
   if (free_delay < 0) free_delay = getenv("PHXMOCK_FREE_DELAY_US") ? atoi(getenv("PHXMOCK_FREE_DELAY_US")) : 0;
   if (sel == PHXN_SEL_BOFREE && free_delay) usleep(free_delay);
   return r;
}
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out, uint32_t *outCnt)
{ return IOConnectCallMethod(c, sel, in, inCnt, NULL, 0, out, outCnt, NULL, NULL); }
kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t sel, const void *in, size_t inCnt, void *out, size_t *outCnt)
{ return IOConnectCallMethod(c, sel, NULL, 0, in, inCnt, NULL, NULL, out, outCnt); }
/* clientMemoryForType: the memory type is the BO handle */
kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options)
{
   (void)task; (void)options;
   if (c != 0x5678 || type == 0 || type >= PHXN_MAX_BOS || !m.bo[type].mem) return kIOReturnNotFound;
   *addr = (mach_vm_address_t)(uintptr_t)m.bo[type].mem; *size = m.bo[type].size;
   return kIOReturnSuccess;
}
kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t addr)
{
   (void)task; (void)addr;
   return c == 0x5678 && type && type < PHXN_MAX_BOS && m.bo[type].mem ? kIOReturnSuccess : kIOReturnNotFound;
}
