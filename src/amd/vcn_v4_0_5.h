/* VCN 4.0.5 (Strix Point's video engine): the pure parts that PhxBringup, mesboot and the test clients share, so the
 * host tests (tests/test_vcn.c) check the very code the hardware gets. Written after Linux amdgpu (vcn_v4_0_5.c,
 * vcn_v2_0.c, amdgpu_vcn.h) and Mesa (ac_vcn.h, ac_cmdbuf_video.c); C99, freestanding, no allocation. */
#ifndef VCN_V4_0_5_H
#define VCN_V4_0_5_H

#include <stdint.h>
#include "vcn_regs_strix.h"

#define VCN45_STACK_SIZE   (128u * 1024)          /* AMDGPU_VCN_STACK_SIZE */
#define VCN45_CONTEXT_SIZE (512u * 1024)          /* AMDGPU_VCN_CONTEXT_SIZE */
#define VCN45_FWSH_SIZE    4096u                  /* AMDGPU_GPU_PAGE_ALIGN(sizeof(struct amdgpu_vcn4_fw_shared)) */
#define VCN45_SRAM_PAIRS   27u                    /* what Linux loads on the 880M: 216 bytes (trace, 2026-10-27) */

/* unified ring commands (amdgpu_vcn.h VCN_ENC_CMD_*) and the ring's alignment (vcn_v4_0_5 align_mask 0x3f) */
#define VCN45_CMD_NOP   0x0u
#define VCN45_CMD_END   0x1u
#define VCN45_CMD_IB    0x2u
#define VCN45_CMD_FENCE 0x3u
#define VCN45_RING_ALIGN_DW 64u

/* The DPG SRAM image vcn_v4_0_5_start_dpg_mode writes with indirect = true on a PSP-loaded part, as (DPG register
 * address, value) pairs: s needs 2 * VCN45_SRAM_PAIRS words. bo_mc is the VCN buffer (stack, then context), fwsh_mc
 * the fw_shared page. VCN_MGCG is set on Strix, so the clock-gating writes are not part of it. Returns words written. */
static inline uint32_t vcn45_dpg_sram(uint32_t *s, uint64_t bo_mc, uint64_t fwsh_mc, uint32_t gb_addr_config)
{
   uint32_t n = 0;
#define VCN45_P(reg, val) do { s[n++] = VCN_DPG_##reg; s[n++] = (uint32_t)(val); } while (0)
   VCN45_P(UVD_VCPU_CNTL, (0xFFu << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT) | UVD_VCPU_CNTL__CLK_EN_MASK | UVD_VCPU_CNTL__BLK_RST_MASK);
   VCN45_P(UVD_MASTINT_EN, 0);
   VCN45_P(UVD_LMI_CTRL, UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK | UVD_LMI_CTRL__REQ_MODE_MASK | UVD_LMI_CTRL__CRC_RESET_MASK |
                         UVD_LMI_CTRL__MASK_MC_URGENT_MASK | UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
                         UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK | (8u << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) | 0x00100000u);
   VCN45_P(UVD_MPC_CNTL, 0x2u << UVD_MPC_CNTL__REPLACEMENT_MODE__SHIFT);
   VCN45_P(UVD_MPC_SET_MUXA0, (1u << UVD_MPC_SET_MUXA0__VARA_1__SHIFT) | (2u << UVD_MPC_SET_MUXA0__VARA_2__SHIFT) |
                              (3u << UVD_MPC_SET_MUXA0__VARA_3__SHIFT) | (4u << UVD_MPC_SET_MUXA0__VARA_4__SHIFT));
   VCN45_P(UVD_MPC_SET_MUXB0, (1u << UVD_MPC_SET_MUXB0__VARB_1__SHIFT) | (2u << UVD_MPC_SET_MUXB0__VARB_2__SHIFT) |
                              (3u << UVD_MPC_SET_MUXB0__VARB_3__SHIFT) | (4u << UVD_MPC_SET_MUXB0__VARB_4__SHIFT));
   VCN45_P(UVD_MPC_SET_MUX, (0u << UVD_MPC_SET_MUX__SET_0__SHIFT) | (1u << UVD_MPC_SET_MUX__SET_1__SHIFT) |
                            (2u << UVD_MPC_SET_MUX__SET_2__SHIFT));
   /* vcn_v4_0_5_mc_resume_dpg_mode, PSP load + indirect: windows 0 (firmware) and 1 (stack) are the PSP's */
   VCN45_P(UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, 0); VCN45_P(UVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, 0);
   VCN45_P(UVD_VCPU_CACHE_OFFSET0, 0); VCN45_P(UVD_VCPU_CACHE_SIZE0, 0);
   VCN45_P(UVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, 0); VCN45_P(UVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, 0);
   VCN45_P(UVD_VCPU_CACHE_OFFSET1, 0); VCN45_P(UVD_VCPU_CACHE_SIZE1, VCN45_STACK_SIZE);
   VCN45_P(UVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, bo_mc + VCN45_STACK_SIZE);
   VCN45_P(UVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH, (bo_mc + VCN45_STACK_SIZE) >> 32);
   VCN45_P(UVD_VCPU_CACHE_OFFSET2, 0); VCN45_P(UVD_VCPU_CACHE_SIZE2, VCN45_CONTEXT_SIZE);
   VCN45_P(UVD_LMI_VCPU_NC0_64BIT_BAR_LOW, fwsh_mc); VCN45_P(UVD_LMI_VCPU_NC0_64BIT_BAR_HIGH, fwsh_mc >> 32);
   VCN45_P(UVD_VCPU_NONCACHE_OFFSET0, 0); VCN45_P(UVD_VCPU_NONCACHE_SIZE0, VCN45_FWSH_SIZE);
   VCN45_P(UVD_GFX10_ADDR_CONFIG, gb_addr_config);
   VCN45_P(UVD_VCPU_CNTL, (0xFFu << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT) | UVD_VCPU_CNTL__CLK_EN_MASK);
   VCN45_P(UVD_LMI_CTRL2, 0x1fu << UVD_LMI_CTRL2__RE_OFLD_MIF_WR_REQ_NUM__SHIFT);
   VCN45_P(UVD_MASTINT_EN, UVD_MASTINT_EN__VCPU_EN_MASK);
#undef VCN45_P
   return n;
}

/* Ring dwords one job takes: n IBs + fence + END, padded to the ring's 64-dword alignment */
static inline uint32_t vcn45_job_dwords(uint32_t n) { return (n * 5u + 5u + VCN45_RING_ALIGN_DW - 1) & ~(VCN45_RING_ALIGN_DW - 1); }

/* One job's packets as Linux's vcn_v2_0_enc_ring_* emit them on VCN 4.0.5: per IB {IB, vmid, va lo, va hi, dwords},
 * then FENCE {address lo, hi, 32-bit sequence} and END, NOP-padded. No TRAP (its interrupt has no taker here; the fence
 * is polled). out needs vcn45_job_dwords(n) words; returns that count. */
static inline uint32_t vcn45_job(uint32_t *out, uint32_t vmid, const uint64_t *ib_va, const uint32_t *ib_dw, uint32_t n,
                                 uint64_t fence_mc, uint32_t seq)
{
   uint32_t k = 0;
   for (uint32_t i = 0; i < n; i++) {
      out[k++] = VCN45_CMD_IB; out[k++] = vmid;
      out[k++] = (uint32_t)ib_va[i]; out[k++] = (uint32_t)(ib_va[i] >> 32); out[k++] = ib_dw[i];
   }
   out[k++] = VCN45_CMD_FENCE; out[k++] = (uint32_t)fence_mc; out[k++] = (uint32_t)(fence_mc >> 32); out[k++] = seq;
   out[k++] = VCN45_CMD_END;
   while (k & (VCN45_RING_ALIGN_DW - 1))
      out[k++] = VCN45_CMD_NOP;
   return k;
}

/* Mesa ac_vcn.h: the unified queue's common engine and its write-memory package (ac_emit_video_write_memory) */
#define VCN45_ENGINE_INFO        0x30000001u
#define VCN45_ENGINE_INFO_SIZE   0x10u
#define VCN45_ENGINE_TYPE_COMMON 0x1u
#define VCN45_OP_WRITEMEMORY     0x33000001u
#define VCN45_WRITE_MEMORY_DW    9u

/* An IB that makes VCN write `value` at `va` (the client's VMID): engine info {size 16, ENGINE_INFO, COMMON, bytes of
 * the engine IB from its size word on}, then the package {size, WRITEMEMORY, address lo, hi, value}. 9 words. */
static inline uint32_t vcn45_write_memory_ib(uint32_t *ib, uint64_t va, uint32_t value)
{
   ib[0] = VCN45_ENGINE_INFO_SIZE; ib[1] = VCN45_ENGINE_INFO; ib[2] = VCN45_ENGINE_TYPE_COMMON;
   ib[3] = VCN45_WRITE_MEMORY_DW * 4;   /* ac_vcn_sq_tail: (end - &ib[3] + 3) dwords, in bytes */
   ib[4] = 8 + 12; ib[5] = VCN45_OP_WRITEMEMORY;
   ib[6] = (uint32_t)va; ib[7] = (uint32_t)(va >> 32); ib[8] = value;
   return VCN45_WRITE_MEMORY_DW;
}

#endif
