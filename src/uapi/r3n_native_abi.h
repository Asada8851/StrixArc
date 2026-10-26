//
//  r3n_native_abi.h — R3N (RDNA3 Native) UserClient ABI specification.
//  Freestanding C/C++ header defining IOConnectCallMethod selectors and structs.
//  Compatible with Mesa RADV Darwin/Linux winsys adapters.
//
#pragma once
#include <stdint.h>

#define R3N_UC_TYPE         0x52334E20u   /* 'R3N ' */
#define R3N_ABI_VERSION     1u
#define R3N_ABI_MINOR       0u
#define R3N_MAX_IBS         64u
#define R3N_MAX_BOS         4096u
#define R3N_MAX_CTX         64u

// Selectors
enum {
    R3N_SEL_HELLO           = 0,
    R3N_SEL_QUERYINFO       = 1,
    R3N_SEL_READREGS        = 2,
    R3N_SEL_BOCREATE        = 3,
    R3N_SEL_BOFREE          = 4,
    R3N_SEL_GEMVA           = 5,
    R3N_SEL_CTX             = 6,
    R3N_SEL_SUBMIT          = 7,
    R3N_SEL_WAITSEQ         = 8,
    R3N_SEL_SCAN_QUERY      = 9,
    R3N_SEL_SCAN_ACQUIRE    = 10,
    R3N_SEL_SCAN_REGISTER   = 11,
    R3N_SEL_SCAN_PRESENT    = 12,
    R3N_SEL_SCAN_STATUS     = 13,
    R3N_SEL_SCAN_RELEASE    = 14,
    R3N_SEL_COUNT           = 15
};

// Memory Domains
#define R3N_GEM_DOMAIN_CPU      0x1u
#define R3N_GEM_DOMAIN_GTT      0x2u
#define R3N_GEM_DOMAIN_VRAM     0x4u

// VM VA Operations
#define R3N_VA_OP_MAP           1u
#define R3N_VA_OP_UNMAP         2u

// VM Flags
#define R3N_VM_PAGE_READABLE    (1u << 1)
#define R3N_VM_PAGE_WRITEABLE   (1u << 2)
#define R3N_VM_PAGE_EXECUTABLE  (1u << 3)

struct r3n_info {
    uint32_t chip_id;            // 0x1900
    uint32_t chip_rev;           // 0xBA
    uint32_t family;             // 143 (AMD_FAMILY_GC_11_0_3)
    uint32_t ip_version_major;   // 11
    uint32_t ip_version_minor;   // 0
    uint32_t ip_version_rev;     // 3
    uint64_t vram_size_bytes;    // 4 GiB
    uint64_t vram_visible_bytes; // 4032 MiB (Full ReBAR) or 256 MiB
    uint64_t gtt_size_bytes;     // System RAM allocation budget
    uint32_t cu_count;           // 12 CUs (Radeon 780M)
    uint32_t max_engine_clock_mhz; // 2700 MHz
    uint32_t max_memory_clock_mhz; // 3200 MHz
    uint32_t display_width;      // 2560
    uint32_t display_height;     // 1600
    uint32_t display_refresh_hz; // 120
    uint32_t reserved[8];
};

struct r3n_gem_create_in {
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint32_t domain;            // R3N_GEM_DOMAIN_*
    uint32_t flags;
};

struct r3n_gem_va {
    uint32_t handle;
    uint32_t op;                // R3N_VA_OP_*
    uint32_t flags;             // R3N_VM_PAGE_*
    uint32_t reserved;
    uint64_t gpu_va;
    uint64_t offset_in_bo;
    uint64_t map_size;
};

struct r3n_ctx {
    uint32_t op;                // 1 = Create, 2 = Destroy
    uint32_t ctx_id;            // in/out
    uint32_t priority;
    uint32_t reserved;
};

struct r3n_cs_ib {
    uint64_t gpu_va;
    uint32_t size_dwords;
    uint32_t ip_type;           // 0 = GFX, 1 = Compute, 2 = SDMA
};

struct r3n_cs_in {
    uint32_t ctx_id;
    uint32_t num_ibs;
    uint64_t reserved;
    r3n_cs_ib ibs[R3N_MAX_IBS];
};

struct r3n_scan_reg {
    uint32_t bo_handle;
    uint32_t width;             // 2560
    uint32_t height;            // 1600
    uint32_t stride_bytes;      // 10240
    uint32_t format;            // 4 = ARGB8888
};

struct r3n_scan_present {
    uint32_t slot_id;
    uint32_t flags;
    uint64_t target_frame;
};
