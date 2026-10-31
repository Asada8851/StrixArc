// mesboot vcn status | mmhub | up <vcn_4_0_5.bin> | ringtest [n] | adopt <vcn_4_0_5.bin> | release | down
// VCN 4.0.5 (the 880M's video codec engine), started the way Linux vcn_v4_0_5 starts it on this chip (traced under
// Linux, tools/linux-trace/trace_vcn.sh, 2026-10-27): the PSP loads the firmware once, then per start the SMU powers
// VCN up (PowerUpVcn0), UVD_POWER_STATUS turns dynamic power gating on, the PSP loads the DPG SRAM image (27 indirect
// register writes, GFX_FW_TYPE_VCN0_RAM) and the unified ring is set up. VCN reaches memory through MMHUB, which
// nothing set up under macOS: `mmhub` programs it as Linux mmhub_v3_3_gart_enable does, with the values Linux leaves on
// this 880M, and points its context 0 at the GART table the GC hub already uses. Included by mesboot.c.
//
// Never read MMVM_INVALIDATE_ENGn_SEM except to take the semaphore and release it at once: a read acquires it (a Linux
// register dump that read ENG0_SEM held it and VCN's next TLB flush waited forever, 2026-10-27). UVD_DPG_LMA_* drive
// indirect access and are not read either. With DPG on, VCN's own registers read 0xdeadbeef while it is gated; the
// ring registers (UVD_RB_*, UVD_POWER_STATUS, VCN_RB_*) sit in the always-on part and read true.
#include "vcn_v4_0_5.h"

#define SEL_DBG_FW_STAGE 104
#define SEL_DBG_PSP_LOADIPFW 105
// VRAM (carve-out offsets): free above the SDMA test's page tables (< 0xC800000) and below the 256 MiB BAR's end
#define VCN_RING   0xE000000ull           // unified ring, 4 KiB = 0x400 dwords
#define VCN_RING_DW 0x400u
#define VCN_FWSH   0xE010000ull           // struct amdgpu_vcn4_fw_shared (148 bytes, one page)
#define VCN_FENCE  0xE011000ull           // PhxBringup's fence for the ring once adopted (build 42)
#define SEL_DBG_ADOPTVCN 114
#define VCN_BO     0xE100000ull           // stack (128 KiB) + context (512 KiB): Linux's VCN BO without the firmware
#define VCN_STACK_SIZE   VCN45_STACK_SIZE
#define VCN_CONTEXT_SIZE VCN45_CONTEXT_SIZE
// fw_buf slots (PSP staging, 16 MiB at VRAM 64 MiB): bringup5.sh fills 0..0x900000 with the GC/SDMA firmware
#define VCN_FWBUF_UCODE 0x900000u
#define VCN_FWBUF_SRAM  0x980000u
#define PSP_FW_TYPE_VCN      13u          // psp_gfx_if.h GFX_FW_TYPE_VCN
#define PSP_FW_TYPE_VCN0_RAM 49u          // GFX_FW_TYPE_VCN0_RAM: the DPG SRAM image
#define SMU_PowerDownVcn0 0x06u           // smu_v14_0_0_ppsmc.h
#define SMU_PowerUpVcn0   0x07u
#define VCN_ENC_CMD_NO_OP VCN45_CMD_NOP
#define VCN_ENC_CMD_END   VCN45_CMD_END
#define VCN_FW_MARK "/var/run/strixarc-vcnfw"   // the firmware is in the TMR this boot (/var/run is cleared at boot)

static uint64_t vcn_fb_start;             // MC address of VRAM offset 0 (MMMC_VM_FB_LOCATION_BASE)
#define VMC(off) (vcn_fb_start + (off))

static void vcn_hdp(void) { if (hdp_flush_dword) { wr_abs(hdp_flush_dword, 0); (void)rd_abs(hdp_flush_dword); } }
static void vcn_vfill(uint64_t off, uint64_t bytes) { for (uint64_t i = 0; i < bytes; i += 4) vw(off + i, 0); }

static int vcn_stage(uint32_t fwbuf_off, const uint8_t *p, uint32_t len)
{
   for (uint32_t done = 0; done < len; ) {
      uint32_t n = len - done > 4096 ? 4096 : len - done;
      uint64_t in[1] = {fwbuf_off + done};
      kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_FW_STAGE, in, 1, p + done, n, NULL, NULL, NULL, NULL);
      if (kr) { printf("vcn: staging at fw_buf+0x%x failed: 0x%x\n", fwbuf_off + done, kr); return -1; }
      done += n;
   }
   return 0;
}
// GFX_CMD_ID_LOAD_IP_FW of what sits at fw_buf + off; 0 when the PSP took it
static int vcn_loadipfw(uint32_t type, uint32_t off, uint32_t size, const char *what)
{
   uint64_t in[3] = {type, off, size}, out[4] = {0, 0, 0, 0}; uint32_t n = 4;
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_LOADIPFW, in, 3, out, &n);
   printf("vcn: PSP LOAD_IP_FW %s (type %u, %u bytes) -> kr 0x%x status 0x%08x, %llu us\n", what, type, size, kr,
          (uint32_t)out[0], (unsigned long long)out[1]);
   return kr || (uint32_t)out[0] ? -1 : 0;
}
// One SMU message on the MP1 mailbox (C2PMSG_66 msg / 82 arg / 90 response), Linux smu_cmn's sequence. PhxBringup's
// SMU selector takes only the bring-up messages, so mesboot writes the mailbox itself, as the Linux trace shows it.
static int vcn_smu(uint32_t msg, uint32_t arg, const char *what)
{
   const uint32_t MSG = 0x16282, ARG = 0x16292, RESP = 0x1629a;
   double t0 = now_ms();
   while (!rd_abs(RESP)) { if (now_ms() - t0 > 2000) { printf("vcn: SMU busy (no response to the last message)\n"); return -1; } usleep(100); }
   wr_abs(RESP, 0); wr_abs(ARG, arg); wr_abs(MSG, msg);
   uint32_t r;
   t0 = now_ms();
   while (!(r = rd_abs(RESP))) { if (now_ms() - t0 > 2000) break; usleep(50); }
   printf("vcn: SMU %s (0x%02x, arg 0x%x) -> 0x%x in %.2f ms\n", what, msg, arg, r, now_ms() - t0);
   return r == 1 ? 0 : -1;
}

static void vcn_status(const char *tag)
{
   printf("vcn status (%s):\n", tag);
   printf("  MMHUB fb 0x%06x..0x%06x off 0x%06x  sysap 0x%x..0x%x  agp 0x%x/0x%x/0x%x  l1tlb 0x%x  l2 0x%x/%x/%x/%x/%x\n",
          rd_abs(MMMC_VM_FB_LOCATION_BASE), rd_abs(MMMC_VM_FB_LOCATION_TOP), rd_abs(MMMC_VM_FB_OFFSET),
          rd_abs(MMMC_VM_SYSTEM_APERTURE_LOW_ADDR), rd_abs(MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR), rd_abs(MMMC_VM_AGP_BASE),
          rd_abs(MMMC_VM_AGP_BOT), rd_abs(MMMC_VM_AGP_TOP), rd_abs(MMMC_VM_MX_L1_TLB_CNTL), rd_abs(MMVM_L2_CNTL),
          rd_abs(MMVM_L2_CNTL2), rd_abs(MMVM_L2_CNTL3), rd_abs(MMVM_L2_CNTL4), rd_abs(MMVM_L2_CNTL5));
   printf("  MMHUB ctx0 cntl 0x%08x pt 0x%08x%08x 0x%08x%08x..0x%08x%08x  ctx1 cntl 0x%08x  fault cntl 0x%08x status 0x%08x addr 0x%x%08x\n",
          rd_abs(MMVM_CONTEXT0_CNTL), rd_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32), rd_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32),
          rd_abs(MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32), rd_abs(MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32),
          rd_abs(MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32), rd_abs(MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32), rd_abs(MMVM_CONTEXT1_CNTL),
          rd_abs(MMVM_L2_PROTECTION_FAULT_CNTL), rd_abs(MMVM_L2_PROTECTION_FAULT_STATUS),
          rd_abs(MMVM_L2_PROTECTION_FAULT_ADDR_HI32), rd_abs(MMVM_L2_PROTECTION_FAULT_ADDR_LO32));
   printf("  GCVM ctx0 cntl 0x%08x pt 0x%08x%08x\n", rd(SEG0(0x1688)), rd(SEG0(0x16f4)), rd(SEG0(0x16f3)));
   printf("  VCN power 0x%08x status 0x%08x  rb 0x%08x%08x size 0x%x rptr 0x%x wptr 0x%x  rb_enable 0x%x db_ctrl 0x%08x  SMU resp 0x%x\n",
          rd_abs(VCN_UVD_POWER_STATUS), rd_abs(VCN_UVD_STATUS), rd_abs(VCN_UVD_RB_BASE_HI), rd_abs(VCN_UVD_RB_BASE_LO),
          rd_abs(VCN_UVD_RB_SIZE), rd_abs(VCN_UVD_RB_RPTR), rd_abs(VCN_UVD_RB_WPTR), rd_abs(VCN_VCN_RB_ENABLE),
          rd_abs(VCN_VCN_RB1_DB_CTRL), rd_abs(0x1629a));
   printf("  fw_shared present 0x%08x queue 0x%08x  firmware %s this boot\n", vr(VCN_FWSH), vr(VCN_FWSH + 16),
          access(VCN_FW_MARK, F_OK) == 0 ? "loaded" : "not loaded");
}

// mmhub_v3_3_gart_enable with the values Linux leaves on this 880M (register dump under Linux, 2026-10-27), and the GC
// hub's GART table as context 0 (Linux shares one GART table between the hubs too). The FB location is the firmware's
// and stays as it is.
static void vcn_mmhub(void)
{
   const uint64_t fb_start = (uint64_t)(rd_abs(MMMC_VM_FB_LOCATION_BASE) & 0xffffff) << 24;
   const uint64_t fb_end = ((uint64_t)(rd_abs(MMMC_VM_FB_LOCATION_TOP) & 0xffffff) << 24) | 0xffffff;
   const uint64_t dummy = FB_PA + DUMMY_PG;
   // context 0 = the GC hub's (mesboot gart at boot): page table base, GART VA range
   wr_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, rd(SEG0(0x16f3))); wr_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, rd(SEG0(0x16f4)));
   wr_abs(MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, rd(SEG0(0x1713))); wr_abs(MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, rd(SEG0(0x1714)));
   wr_abs(MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, rd(SEG0(0x1733))); wr_abs(MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, rd(SEG0(0x1734)));
   // system aperture: AGP off, the FB range, scratch and dummy page = the experiment's dummy page (as gart() for GC)
   wr_abs(MMMC_VM_AGP_BASE, 0); wr_abs(MMMC_VM_AGP_BOT, 0x00ffffff); wr_abs(MMMC_VM_AGP_TOP, 0);
   wr_abs(MMMC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(fb_start >> 18)); wr_abs(MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(fb_end >> 18));
   wr_abs(MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (uint32_t)(dummy >> 12)); wr_abs(MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (uint32_t)(dummy >> 44));
   wr_abs(MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (uint32_t)(dummy >> 12)); wr_abs(MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (uint32_t)(dummy >> 44));
   wr_abs(MMVM_L2_PROTECTION_FAULT_CNTL2, 0x000e0000);
   wr_abs(MMVM_L2_PROTECTION_FAULT_CNTL, 0x3ffffffc);          // faults go to the dummy page (Linux, and our GC hub)
   wr_abs(MMMC_VM_MX_L1_TLB_CNTL, 0x1859);
   wr_abs(MMVM_L2_CNTL, 0x00080e01); wr_abs(MMVM_L2_CNTL2, 0x00000003); wr_abs(MMVM_L2_CNTL3, 0x80130009);
   wr_abs(MMVM_L2_CNTL4, 0x00000001); wr_abs(MMVM_L2_CNTL5, 0x00003fe0);
   wr_abs(MMVM_CONTEXT0_CNTL, 0x01fffe01);
   wr_abs(MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0xffffffff); wr_abs(MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0xf);
   wr_abs(MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0); wr_abs(MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
   wr_abs(MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0); wr_abs(MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
   for (uint32_t c = 1; c < 16; c++) {                         // VMID contexts as Linux enables them (tables come per job)
      wr_abs(MMVM_CONTEXT1_CNTL + (c - 1), 0x01fffe07);
      wr_abs(MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 + 2 * (c - 1), 0); wr_abs(MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32 + 2 * (c - 1), 0);
      wr_abs(MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32 + 2 * (c - 1), 0xffffffff); wr_abs(MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32 + 2 * (c - 1), 0xf);
   }
   for (uint32_t e = 0; e < 18; e++) {
      wr_abs(MMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 + 2 * e, 0xffffffff); wr_abs(MMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 + 2 * e, 0x1f);
   }
   // flush VMID 0 on engine 17 under the hub's semaphore, as Linux's gmc_v11 does on MMHUB: the read takes it, the
   // write of 0 gives it back - in the same function, every time
   int sem = 0;
   for (int i = 0; i < 1000 && !(sem = rd_abs(MMVM_INVALIDATE_ENG17_SEM) & 1); i++) usleep(10);
   // mmhub_v3_3_get_invalidate_req: VMID 0, FLUSH_TYPE 1 (Linux turns 0 into 1 on MMHUB), L2 PTEs/PDE0-2 and L1 PTEs.
   // The GC hub's 0x00F80001 (flush type 0) went unacknowledged here (round 49).
   wr_abs(MMVM_INVALIDATE_ENG17_REQ, 0x00F90001);
   uint32_t ack = 0; double t0 = now_ms();
   while (!((ack = rd_abs(MMVM_INVALIDATE_ENG17_ACK)) & 1) && now_ms() - t0 < 100) usleep(10);
   if (sem) wr_abs(MMVM_INVALIDATE_ENG17_SEM, 0);
   printf("vcn: MMHUB set up: fb 0x%llx..0x%llx, context 0 = the GC hub's GART (pt 0x%08x%08x), flush %s (semaphore %s)\n",
          (unsigned long long)fb_start, (unsigned long long)fb_end, rd_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32),
          rd_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32), ack & 1 ? "acked" : "NOT acked", sem ? "taken and released" : "not taken");
}
static int vcn_mmhub_ready(void)
{
   return (rd_abs(MMVM_L2_CNTL) & 1) && (rd_abs(MMVM_CONTEXT0_CNTL) & 1) &&
          rd_abs(MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32) == rd(SEG0(0x16f3)) &&
          rd_abs(MMVM_L2_PROTECTION_FAULT_CNTL) == 0x3ffffffc;
}

static int vcn_load_fw(const char *path)
{
   if (access(VCN_FW_MARK, F_OK) == 0) { printf("vcn: firmware already loaded this boot\n"); return 0; }
   FILE *f = path ? fopen(path, "rb") : NULL;
   if (!f) { printf("vcn: cannot read the firmware %s\n", path ? path : "(none given)"); return -1; }
   static uint8_t bin[1 << 20];
   size_t len = fread(bin, 1, sizeof bin, f); fclose(f);
   // common_firmware_header: ucode_size_bytes at +20, ucode_array_offset_bytes at +24 (amdgpu_ucode.h), as Linux cuts it
   uint32_t size, off;
   if (len < 32) { printf("vcn: %s is too short\n", path); return -1; }
   memcpy(&size, bin + 20, 4); memcpy(&off, bin + 24, 4);
   if (!size || (uint64_t)off + size > len || size > 0x80000) { printf("vcn: bad header in %s (offset %u size %u of %zu)\n", path, off, size, len); return -1; }
   if (vcn_stage(VCN_FWBUF_UCODE, bin + off, size) || vcn_loadipfw(PSP_FW_TYPE_VCN, VCN_FWBUF_UCODE, size, "VCN firmware")) return -1;
   FILE *m = fopen(VCN_FW_MARK, "w"); if (m) fclose(m);
   return 0;
}

static void vcn_fwsh_queue(uint8_t mode) { uint32_t w = vr(VCN_FWSH + 16); vw(VCN_FWSH + 16, (w & ~0xff00u) | ((uint32_t)mode << 8)); vcn_hdp(); }

static int vcn_up(const char *fw)
{
   if (!vcn_mmhub_ready()) { printf("vcn: MMHUB not set up for VCN yet\n"); vcn_mmhub(); }
   if (vcn_load_fw(fw)) return 2;
   // fw_shared as vcn_v4_0_5_sw_init fills it: unified queue on, SMU DPM interface APU, DRM key workaround 2
   vcn_vfill(VCN_FWSH, 4096);
   vw(VCN_FWSH + 0, (1u << 2) | (1u << 11) | (1u << 4));   // UNIFIED_QUEUE | SMU_DPM_INTERFACE | DRM_KEY_INJECT
   vw(VCN_FWSH + 16, 1);                                    // sq.is_enabled = 1 (queue_mode at +17 = 0)
   vw(VCN_FWSH + 120, 1);                                   // smu_dpm_interface.smu_interface_type = APU
   vw(VCN_FWSH + 124, 2);                                   // drm_key_wa.method = VCNFW_ASD_HANDSHAKING
   vcn_vfill(VCN_RING, VCN_RING_DW * 4);
   if (!env_on("VCN_NO_BOZERO")) vcn_vfill(VCN_BO, VCN_STACK_SIZE + VCN_CONTEXT_SIZE);
   vcn_hdp();
   if (vcn_smu(SMU_PowerUpVcn0, 0, "PowerUpVcn0")) return 3;
   // anti-hang off, then dynamic power gating on (Linux: 1, then 0x105)
   uint32_t t = rd_abs(VCN_UVD_POWER_STATUS);
   wr_abs(VCN_UVD_POWER_STATUS, (t & ~UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) | 1);
   t = rd_abs(VCN_UVD_POWER_STATUS) | UVD_POWER_STATUS__UVD_PG_MODE_MASK | UVD_POWER_STATUS__UVD_PG_EN_MASK;
   wr_abs(VCN_UVD_POWER_STATUS, t);
   // the DPG SRAM image through the PSP
   uint32_t sram[64];
   const uint32_t gb = rd_abs(0x1260 + 0x13de);             // GB_ADDR_CONFIG (GC 11.5 segment 0)
   const uint32_t nw = vcn45_dpg_sram(sram, VMC(VCN_BO), VMC(VCN_FWSH), gb);
   printf("vcn: DPG SRAM %u pairs (%u bytes; Linux loads 216), gb_addr_config 0x%x, bo mc 0x%llx, fw_shared mc 0x%llx\n",
          nw / 2, nw * 4, gb, (unsigned long long)VMC(VCN_BO), (unsigned long long)VMC(VCN_FWSH));
   if (env_on("VCN_VERBOSE")) for (uint32_t i = 0; i < nw; i += 2) printf("   sram %2u: 0x%05x <- 0x%08x\n", i / 2, sram[i], sram[i + 1]);
   if (vcn_stage(VCN_FWBUF_SRAM, (const uint8_t *)sram, nw * 4) || vcn_loadipfw(PSP_FW_TYPE_VCN0_RAM, VCN_FWBUF_SRAM, nw * 4, "DPG SRAM"))
      return 4;
   // the unified ring (vcn_v4_0_5_start_dpg_mode's tail), no doorbell: the write pointer goes through UVD_RB_WPTR
   wr_abs(VCN_UVD_RB_BASE_LO, (uint32_t)VMC(VCN_RING)); wr_abs(VCN_UVD_RB_BASE_HI, (uint32_t)(VMC(VCN_RING) >> 32));
   wr_abs(VCN_UVD_RB_SIZE, VCN_RING_DW);
   wr_abs(VCN_VCN_RB_ENABLE, rd_abs(VCN_VCN_RB_ENABLE) & ~VCN_RB_ENABLE__RB1_EN_MASK);
   vcn_fwsh_queue(1);                                       // FW_QUEUE_RING_RESET
   wr_abs(VCN_UVD_RB_RPTR, 0); wr_abs(VCN_UVD_RB_WPTR, 0);
   wr_abs(VCN_UVD_RB_WPTR, rd_abs(VCN_UVD_RB_RPTR));
   wr_abs(VCN_VCN_RB_ENABLE, rd_abs(VCN_VCN_RB_ENABLE) | VCN_RB_ENABLE__RB1_EN_MASK);
   vcn_fwsh_queue(0);                                       // RING_RESET and DPG_HOLD_OFF cleared
   wr_abs(VCN_VCN_RB1_DB_CTRL, 0);
   (void)rd_abs(VCN_VCN_RB1_DB_CTRL);
   printf("vcn: up: ring at mc 0x%llx, %u dwords, rptr 0x%x wptr 0x%x, power 0x%x\n", (unsigned long long)VMC(VCN_RING), VCN_RING_DW,
          rd_abs(VCN_UVD_RB_RPTR), rd_abs(VCN_UVD_RB_WPTR), rd_abs(VCN_UVD_POWER_STATUS));
   return 0;
}

// amdgpu_vcn_enc_ring_test_ring: VCN_ENC_CMD_END, padded with NOPs to 64 dwords (align_mask 0x3f); the read pointer moves
static int vcn_ringtest(int times)
{
   int ok = 0;
   for (int k = 0; k < times; k++) {
      const uint32_t rptr = rd_abs(VCN_UVD_RB_RPTR), wptr = rd_abs(VCN_UVD_RB_WPTR) % VCN_RING_DW;
      vw(VCN_RING + 4ull * wptr, VCN_ENC_CMD_END);
      for (uint32_t i = 1; i < 64; i++) vw(VCN_RING + 4ull * ((wptr + i) % VCN_RING_DW), VCN_ENC_CMD_NO_OP);
      vcn_hdp();
      const uint32_t nw = (wptr + 64) % VCN_RING_DW;
      const double t0 = now_ms();
      wr_abs(VCN_UVD_RB_WPTR, nw);
      uint32_t r = rptr;
      while ((r = rd_abs(VCN_UVD_RB_RPTR)) == rptr && now_ms() - t0 < 1000) usleep(20);
      const int moved = r != rptr;
      ok += moved;
      printf("vcn: ring test %d: rptr 0x%x -> 0x%x (wptr 0x%x) %s in %.2f ms, power 0x%x, MMHUB fault status 0x%x\n", k + 1, rptr, r,
             nw, moved ? "MOVED" : "did not move", now_ms() - t0, rd_abs(VCN_UVD_POWER_STATUS), rd_abs(MMVM_L2_PROTECTION_FAULT_STATUS));
      if (!moved) break;
   }
   printf("vcn: ring test %s (%d of %d)\n", ok == times ? "PASSED" : "FAILED", ok, times);
   return ok == times ? 0 : 5;
}

// vcn_v4_0_5_stop_dpg_mode, then the SMU powers VCN down
static int vcn_down(void)
{
   double t0 = now_ms();
   while ((rd_abs(VCN_UVD_POWER_STATUS) & UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) != 1 && now_ms() - t0 < 500) usleep(100);
   const uint32_t wptr = rd_abs(VCN_UVD_RB_WPTR);
   while (rd_abs(VCN_UVD_RB_RPTR) != wptr && now_ms() - t0 < 1000) usleep(100);
   const uint32_t ps = rd_abs(VCN_UVD_POWER_STATUS);
   wr_abs(VCN_UVD_POWER_STATUS, ps & ~UVD_POWER_STATUS__UVD_PG_MODE_MASK);
   printf("vcn: stopped (power 0x%x -> 0x%x, rptr 0x%x wptr 0x%x)\n", ps, rd_abs(VCN_UVD_POWER_STATUS), rd_abs(VCN_UVD_RB_RPTR), wptr);
   return vcn_smu(SMU_PowerDownVcn0, 0, "PowerDownVcn0") ? 3 : 0;
}

// Hand the running ring to PhxBringup 42+ (AMDGPU_HW_IP_VCN_ENC submissions from clients), or take it back (`release`,
// before `down`). mesboot does not touch the ring while the kext has it.
static int vcn_adopt(const char *fw)
{
   uint32_t ver = 0;
   FILE *f = fw ? fopen(fw, "rb") : NULL;
   if (f) { uint8_t h[32]; if (fread(h, 1, 32, f) == 32) memcpy(&ver, h + 16, 4); fclose(f); }   // ucode_version
   vw(VCN_FENCE, 0); vw(VCN_FENCE + 4, 0); vcn_hdp();
   uint64_t in[5] = {VCN_RING, VCN_RING_DW, VCN_FENCE, rd_abs(VCN_UVD_RB_WPTR) % VCN_RING_DW, ver};
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTVCN, in, 5, NULL, NULL);
   printf("vcn: adopt (ring mc 0x%llx, wptr 0x%llx, firmware 0x%08x) -> 0x%x%s\n", (unsigned long long)VMC(VCN_RING),
          (unsigned long long)in[3], ver, kr, kr == kIOReturnBadArgument ? " (PhxBringup older than build 42?)" : "");
   return kr ? 6 : 0;
}
static int vcn_release(void)
{
   uint64_t in[5] = {0, 0, 0, 0, 0};
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTVCN, in, 5, NULL, NULL);
   printf("vcn: release -> 0x%x%s\n", kr, kr == kIOReturnBusy ? " (jobs still running)" : "");
   return kr ? 6 : 0;
}

static int vcn_cmd(int argc, char **argv)
{
   if (chip->id != RDNA3_CHIP_STRIX_POINT) { fprintf(stderr, "mesboot vcn: Strix Point (VCN 4.0.5) only\n"); return 1; }
   vcn_fb_start = (uint64_t)(rd_abs(MMMC_VM_FB_LOCATION_BASE) & 0xffffff) << 24;
   const char *op = argc > 2 ? argv[2] : "status";
   if (!strcmp(op, "status")) { vcn_status("now"); return 0; }
   if (!strcmp(op, "mmhub")) { vcn_mmhub(); vcn_status("after mmhub"); return 0; }
   if (!strcmp(op, "up")) { int r = vcn_up(argc > 3 ? argv[3] : NULL); vcn_status(r ? "up FAILED" : "after up"); return r; }
   if (!strcmp(op, "ringtest")) return vcn_ringtest(argc > 3 ? atoi(argv[3]) : 3);
   if (!strcmp(op, "adopt")) return vcn_adopt(argc > 3 ? argv[3] : NULL);
   if (!strcmp(op, "release")) return vcn_release();
   if (!strcmp(op, "down")) { int r = vcn_down(); vcn_status("after down"); return r; }
   fprintf(stderr, "mesboot vcn status | mmhub | up <vcn_4_0_5.bin> | ringtest [n] | adopt <vcn_4_0_5.bin> | release | down\n");
   return 2;
}
