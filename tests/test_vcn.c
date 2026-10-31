/* Host tests of src/amd/vcn_v4_0_5.h (what PhxBringup, mesboot and vcnjob send VCN), checked against sources other
 * than the code itself: register addresses Linux used on the 880M (tools/linux-trace/trace_vcn.sh, its register
 * snapshot), SOC15_DPG_MODE_OFFSET re-implemented from amdgpu_vcn.h, the SRAM size of Linux's PSP load (216 bytes),
 * the packet layout of vcn_v2_0_enc_ring_emit_ib/_fence/insert_end, and Mesa's ac_vcn_sq_header/_tail. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vcn_v4_0_5.h"
#include <assert.h>
#include "PhxNativeABI.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* amdgpu_vcn.h SOC15_DPG_MODE_OFFSET for instance 0 of a VCN 4.0.5 (vcn_v4_0_5.c's VID/AON window constants) */
static uint32_t dpg_offset(uint32_t absreg)
{
   const uint32_t vid = 0x1FB00, aon = 0x1F800, aon_ip = 0x30000;
   uint32_t a = (absreg << 2) & 0xFFFFF;
   if (a >= vid && a < vid + 0x2600) a = a - vid;
   else if (a >= aon && a < aon + 0x600) a = a - aon + aon_ip;
   return a >> 2;
}

static void test_addresses(void)
{
   /* register writes/reads in the Linux trace of the 880M (amdgpu_device_wreg/rreg) */
   CHECK(VCN_UVD_POWER_STATUS == 0x7e02, "UVD_POWER_STATUS 0x%x", VCN_UVD_POWER_STATUS);
   CHECK(VCN_UVD_RB_BASE_LO == 0x7eea && VCN_UVD_RB_BASE_HI == 0x7eeb && VCN_UVD_RB_SIZE == 0x7eec, "UVD_RB_BASE/SIZE");
   CHECK(VCN_UVD_RB_RPTR == 0x7eac && VCN_UVD_RB_WPTR == 0x7ead, "UVD_RB_RPTR/WPTR");
   CHECK(VCN_VCN_RB_ENABLE == 0x7e85 && VCN_VCN_RB1_DB_CTRL == 0x7e72, "VCN_RB_ENABLE / VCN_RB1_DB_CTRL");
   CHECK(VCN_UVD_STATUS == 0x7f43, "UVD_STATUS");
   /* the Linux register snapshot of MMHUB (debugfs amdgpu_regs) */
   CHECK(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 == 0x1a8ab && MMVM_CONTEXT0_CNTL == 0x1a840, "MMHUB context 0");
   CHECK(MMMC_VM_FB_LOCATION_BASE == 0x1a9ec && MMMC_VM_FB_OFFSET == 0x1a9d7, "MMHUB FB location");
   CHECK(MMVM_L2_PROTECTION_FAULT_CNTL == 0x1a808 && MMVM_L2_CNTL == 0x1a800, "MMHUB L2");
   CHECK(MMMC_VM_SYSTEM_APERTURE_LOW_ADDR == 0x1a9f1, "MMHUB system aperture");
   /* contexts are 2 dwords apart (LO/HI), as the kext's per-VMID arithmetic assumes */
   CHECK(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 == MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + 1, "PT base HI after LO");
   CHECK(MMVM_CONTEXT1_CNTL == MMVM_CONTEXT0_CNTL + 1, "CNTL stride 1");
   CHECK(MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 == MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 + 2, "PT start stride 2");
   CHECK(MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32 == MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32 + 2, "PT end stride 2");
}

static void test_sram(void)
{
   uint32_t s[2 * VCN45_SRAM_PAIRS + 8];
   memset(s, 0xA5, sizeof s);
   const uint64_t bo = 0x800E100000ull, fw = 0x800E010000ull;
   const uint32_t n = vcn45_dpg_sram(s, bo, fw, 0x242);
   CHECK(n == 2 * VCN45_SRAM_PAIRS && n * 4 == 216, "SRAM %u words, Linux loads 216 bytes", n);
   CHECK(s[n] == 0xA5A5A5A5u, "SRAM writes past its end");
   /* every DPG address is the SOC15_DPG_MODE_OFFSET of a VCN register address */
   static const uint32_t order[] = {
      VCN_UVD_VCPU_CNTL, VCN_UVD_MASTINT_EN, VCN_UVD_LMI_CTRL, VCN_UVD_MPC_CNTL, VCN_UVD_MPC_SET_MUXA0, VCN_UVD_MPC_SET_MUXB0,
      VCN_UVD_MPC_SET_MUX, VCN_UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, VCN_UVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
      VCN_UVD_VCPU_CACHE_OFFSET0, VCN_UVD_VCPU_CACHE_SIZE0, VCN_UVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW,
      VCN_UVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, VCN_UVD_VCPU_CACHE_OFFSET1, VCN_UVD_VCPU_CACHE_SIZE1,
      VCN_UVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, VCN_UVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH, VCN_UVD_VCPU_CACHE_OFFSET2,
      VCN_UVD_VCPU_CACHE_SIZE2, VCN_UVD_LMI_VCPU_NC0_64BIT_BAR_LOW, VCN_UVD_LMI_VCPU_NC0_64BIT_BAR_HIGH,
      VCN_UVD_VCPU_NONCACHE_OFFSET0, VCN_UVD_VCPU_NONCACHE_SIZE0, VCN_UVD_GFX10_ADDR_CONFIG, VCN_UVD_VCPU_CNTL,
      VCN_UVD_LMI_CTRL2, VCN_UVD_MASTINT_EN };   /* vcn_v4_0_5_start_dpg_mode's order */
   CHECK(sizeof order / sizeof order[0] == VCN45_SRAM_PAIRS, "order table");
   for (uint32_t i = 0; i < VCN45_SRAM_PAIRS; i++)
      CHECK(s[2 * i] == dpg_offset(order[i]), "pair %u: DPG address 0x%x, want 0x%x (reg 0x%x)", i, s[2 * i], dpg_offset(order[i]), order[i]);
   /* values that carry addresses and sizes */
   CHECK(s[2 * 15 + 1] == (uint32_t)(bo + VCN45_STACK_SIZE) && s[2 * 16 + 1] == (uint32_t)((bo + VCN45_STACK_SIZE) >> 32), "cache 2 = context after the stack");
   CHECK(s[2 * 14 + 1] == 128 * 1024 && s[2 * 18 + 1] == 512 * 1024, "stack/context sizes");
   CHECK(s[2 * 19 + 1] == (uint32_t)fw && s[2 * 20 + 1] == (uint32_t)(fw >> 32) && s[2 * 22 + 1] == 4096, "non-cache window = fw_shared page");
   CHECK(s[2 * 23 + 1] == 0x242, "GFX10_ADDR_CONFIG");
   CHECK((s[1] & UVD_VCPU_CNTL__BLK_RST_MASK) && !(s[2 * 24 + 1] & UVD_VCPU_CNTL__BLK_RST_MASK), "VCPU block reset on, then off");
   CHECK(s[2 * 26 + 1] == UVD_MASTINT_EN__VCPU_EN_MASK && s[3] == 0, "master interrupt off, then VCPU_EN");
   CHECK(s[2 * 7 + 1] == 0 && s[2 * 11 + 1] == 0, "windows 0 and 1 left to the PSP");
}

static void test_job(void)
{
   uint32_t out[64 * 5 + 5 + 64];
   const uint64_t va[2] = {0x0000123456789000ull, 0x7FFF00001000ull};
   const uint32_t dw[2] = {9, 300};
   uint32_t k = vcn45_job(out, 9, va, dw, 1, 0x800E011000ull, 77);
   const uint32_t want[] = {2, 9, 0x56789000u, 0x1234, 9, 3, 0x0E011000u, 0x80, 77, 1};
   CHECK(k == 64 && k == vcn45_job_dwords(1), "1 IB: %u dwords", k);
   for (unsigned i = 0; i < sizeof want / 4; i++) CHECK(out[i] == want[i], "1 IB word %u: 0x%x want 0x%x", i, out[i], want[i]);
   for (unsigned i = sizeof want / 4; i < k; i++) CHECK(out[i] == 0, "padding word %u not NOP", i);
   k = vcn45_job(out, 15, va, dw, 2, 0x800E011000ull, 0xFFFFFFFFu);
   CHECK(k == 64 && out[5] == 2 && out[6] == 15 && out[9] == 300 && out[10] == 3 && out[13] == 0xFFFFFFFFu && out[14] == 1, "2 IBs");
   CHECK(vcn45_job_dwords(12) == 128 && vcn45_job_dwords(11) == 64 && vcn45_job_dwords(64) == 384, "job sizes");
   /* the kext's stack buffer (PHXN_MAX_IBS 64) holds the largest job */
   CHECK(vcn45_job_dwords(64) <= 64 * 5 + 5 + VCN45_RING_ALIGN_DW, "kext buffer");
   k = vcn45_job(out, 8, va, dw, 0, 0x1000, 1);
   CHECK(k == 64 && out[0] == 3, "0 IBs: just the fence");
}

/* Mesa's vcn_common_cmd + ac_vcn_sq_header/tail, re-done here: engine info, package header, payload, size patched */
static uint32_t mesa_write_memory(uint32_t *buf, uint64_t va, uint32_t value)
{
   uint32_t cdw = 0;
   buf[cdw++] = 0x10; buf[cdw++] = 0x30000001; buf[cdw++] = 1; buf[cdw++] = 0;
   uint32_t *sz = &buf[cdw - 1];
   buf[cdw++] = 8 + 12; buf[cdw++] = 0x33000001;
   buf[cdw++] = (uint32_t)va; buf[cdw++] = (uint32_t)(va >> 32); buf[cdw++] = value;
   *sz = (uint32_t)((&buf[cdw] - sz) + 3) * 4;
   return cdw;
}

static void test_write_memory(void)
{
   uint32_t a[16], b[16];
   memset(a, 0, sizeof a); memset(b, 0, sizeof b);
   const uint32_t na = vcn45_write_memory_ib(a, 0x201004ull, 0xCAFE);
   const uint32_t nb = mesa_write_memory(b, 0x201004ull, 0xCAFE);
   CHECK(na == nb && na == 9, "sizes %u %u", na, nb);
   for (unsigned i = 0; i < 9; i++) CHECK(a[i] == b[i], "word %u: 0x%x, Mesa 0x%x", i, a[i], b[i]);
}

static void test_abi(void)
{
   /* amdgpu_drm.h numbering (Linux and Mesa): 4 is VCE, 7 the VCN unified queue RADV submits to; build 42 said 4 */
   CHECK(PHX_AMDGPU_HW_IP_VCN_ENC == 7, "PHX_AMDGPU_HW_IP_VCN_ENC %u", PHX_AMDGPU_HW_IP_VCN_ENC);
   CHECK(PHX_AMDGPU_HW_IP_GFX == 0 && PHX_AMDGPU_HW_IP_COMPUTE == 1, "GFX/COMPUTE");
}

int main(void)
{
   test_abi();
   test_addresses();
   test_sram();
   test_job();
   test_write_memory();
   printf("test_vcn: %s\n", fails ? "FAILED" : "all passed");
   return fails != 0;
}
