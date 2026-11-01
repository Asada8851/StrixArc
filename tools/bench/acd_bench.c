/* acd_bench: Mesa's RADV Darwin backend (src/amd/common/darwin, our patch series applied) run on the Linux bench
 * against phxmock, through the same entry points RADV's winsys uses: device init, the INFO queries ac_query_gpu_info and
 * ac_fill_video_info make for the video engine, and a VCN command stream (DRM_AMDGPU_CS with an IB on
 * AMDGPU_HW_IP_VCN_ENC) that phxmock executes. PHXMOCK_NOVCN=1: the kext reports no VCN; the queue must vanish.
 * Exit 0 when every check holds. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ac_darwin_drm.h"
#include "drm-uapi/amdgpu_drm.h"
#include "vcn_v4_0_5.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int info(acd_device_handle d, struct drm_amdgpu_info *q, void *out, unsigned size)
{
   q->return_pointer = (uintptr_t)out; q->return_size = size;
   return acd_ioctl(d, DRM_AMDGPU_INFO, q, sizeof *q);
}

#define STRESS_N 2000
static void *stress_thread(void *arg)
{
   struct { acd_device_handle d; int fails; } *st = arg;
   for (int i = 0; i < STRESS_N; i++) {
      struct amdgpu_bo_alloc_request req = {.alloc_size = 4096, .phys_alignment = 4096, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT};
      amdgpu_bo_handle bo = NULL;
      if (acd_bo_alloc(st->d, &req, &bo) || !bo) { st->fails++; continue; }
      acd_bo_free(st->d, bo);
   }
   return NULL;
}

int main(void)
{
   const int vcn = !getenv("PHXMOCK_NOVCN");
   uint32_t maj = 0, min = 0;
   acd_device_handle d = NULL;
   int r = acd_device_initialize(&maj, &min, &d);
   if (r || !d) { printf("FAIL: acd_device_initialize -> %d\n", r); return 1; }
   printf("acd_bench: device up (drm %u.%u), %s, mock VCN %s\n", maj, min, acd_get_marketing_name(d), vcn ? "on" : "off");

   /* ac_query_gpu_info: HW_IP_INFO for the unified queue */
   struct drm_amdgpu_info q; struct drm_amdgpu_info_hw_ip ip;
   memset(&q, 0, sizeof q); memset(&ip, 0, sizeof ip);
   q.query = AMDGPU_INFO_HW_IP_INFO; q.query_hw_ip.type = AMDGPU_HW_IP_VCN_ENC;
   r = info(d, &q, &ip, sizeof ip);
   CHECK(!r, "HW_IP_INFO -> %d", r);
   if (vcn)
      CHECK(ip.available_rings == 1 && ip.ip_discovery_version == 0x040005 && ip.hw_ip_version_major == 4 &&
            ip.ib_start_alignment == 256 && ip.ib_size_alignment == 4,
            "VCN_ENC ip: rings 0x%x disc 0x%x %u.%u align %u/%u", ip.available_rings, ip.ip_discovery_version,
            ip.hw_ip_version_major, ip.hw_ip_version_minor, ip.ib_start_alignment, ip.ib_size_alignment);
   else
      CHECK(ip.available_rings == 0, "no VCN but rings 0x%x", ip.available_rings);
   /* the GFX queue is still there either way */
   memset(&ip, 0, sizeof ip); q.query_hw_ip.type = AMDGPU_HW_IP_GFX;
   CHECK(!info(d, &q, &ip, sizeof ip) && ip.available_rings, "GFX ip gone");

   /* firmware version (ac_gpu_info reads ENC 1.24 out of it: write_memory needs minor >= 22) */
   struct drm_amdgpu_info_firmware fw = {0, 0};
   memset(&q, 0, sizeof q); q.query = AMDGPU_INFO_FW_VERSION; q.query_fw.fw_type = AMDGPU_INFO_FW_VCN;
   r = info(d, &q, &fw, sizeof fw);
   if (vcn) CHECK(!r && fw.ver == 0x09118022 && ((fw.ver >> 12) & 0xff) >= 22, "FW_VCN -> %d ver 0x%x", r, fw.ver);
   else CHECK(r != 0, "FW_VCN answered without VCN");

   /* video caps: every slot written; decode AVC/HEVC as amdgpu reports them */
   struct drm_amdgpu_info_video_caps caps;
   for (int enc = 0; enc < 2; enc++) {
      memset(&caps, 0xA5, sizeof caps);
      memset(&q, 0, sizeof q); q.query = AMDGPU_INFO_VIDEO_CAPS; q.sensor_info.type = enc;   /* as ac_drm_query_video_caps_info */
      r = info(d, &q, &caps, sizeof caps);
      CHECK(!r, "VIDEO_CAPS %d -> %d", enc, r);
      for (int i = 0; i < AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT; i++)
         CHECK(caps.codec_info[i].valid == 0 || caps.codec_info[i].valid == 1, "caps %d slot %d valid 0x%x (left untouched?)", enc, i, caps.codec_info[i].valid);
      const struct drm_amdgpu_info_video_codec_info *avc = &caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC];
      const struct drm_amdgpu_info_video_codec_info *hevc = &caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC];
      if (vcn)
         CHECK(avc->valid && avc->max_width == 4096 && hevc->valid && hevc->max_width == 8192 && hevc->max_height == 4352 &&
               (enc || (avc->max_level == 52 && hevc->max_level == 186)) && !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG2].valid,
               "caps %d: avc %u %ux%u hevc %u %ux%u", enc, avc->valid, avc->max_width, avc->max_height, hevc->valid, hevc->max_width, hevc->max_height);
      else
         CHECK(!avc->valid && !hevc->valid, "caps %d without VCN", enc);
   }

   /* a command stream the way radv_amdgpu_cs submits one: BO, VA, context, CS with one IB chunk */
   struct amdgpu_bo_alloc_request req = {.alloc_size = 65536, .phys_alignment = 4096, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT};
   amdgpu_bo_handle bo = NULL; void *cpu = NULL; uint32_t kms = 0;
   CHECK(!acd_bo_alloc(d, &req, &bo) && bo, "bo alloc");
   CHECK(!acd_bo_cpu_map(d, bo, &cpu) && cpu, "bo map");
   CHECK(!acd_bo_export(d, bo, amdgpu_bo_handle_type_kms, &kms) && kms, "bo export kms");
   const uint64_t va = 0x400000;
   struct drm_amdgpu_gem_va gv = {.handle = kms, .operation = AMDGPU_VA_OP_MAP,
                                  .flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
                                  .va_address = va, .offset_in_bo = 0, .map_size = 65536};
   r = acd_ioctl(d, DRM_AMDGPU_GEM_VA, &gv, sizeof gv);
   CHECK(!r, "GEM_VA -> %d", r);
   union drm_amdgpu_ctx ctx; memset(&ctx, 0, sizeof ctx); ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
   r = acd_ioctl(d, DRM_AMDGPU_CTX, &ctx, sizeof ctx);
   CHECK(!r && ctx.out.alloc.ctx_id, "CTX -> %d id %u", r, ctx.out.alloc.ctx_id);
   uint32_t *m = cpu;
   if (!cpu || r) { printf("acd_bench: FAILED (no BO/context)\n"); return 1; }

   /* every ring a queue family is built from must be one the kext takes: RADV hands out one Vulkan queue per bit of
    * available_rings and submits on ring = queue index. Round 53 (2026-10-28): compute advertised 0xf, the kext takes
    * ring 0 only, FFmpeg's copies on queue 1..3 came back EINVAL. A CP IB of type-2 NOPs (the mock does not run it). */
   static const struct { uint32_t ip; const char *name; } eng[] = {
      {AMDGPU_HW_IP_GFX, "GFX"}, {AMDGPU_HW_IP_COMPUTE, "COMPUTE"}, {AMDGPU_HW_IP_VCN_ENC, "VCN_ENC"}};
   for (unsigned e = 0; e < sizeof eng / sizeof eng[0]; e++) {
      memset(&q, 0, sizeof q); memset(&ip, 0, sizeof ip);
      q.query = AMDGPU_INFO_HW_IP_INFO; q.query_hw_ip.type = eng[e].ip;
      CHECK(!info(d, &q, &ip, sizeof ip), "HW_IP_INFO %s", eng[e].name);
      for (uint32_t ring = 0; ring < 32; ring++) {
         if (!(ip.available_rings & (1u << ring)))
            continue;
         uint32_t dw = 8;
         if (eng[e].ip == AMDGPU_HW_IP_VCN_ENC) {
            m[2048] = 0;
            vcn45_write_memory_ib(m, va + 4ull * 2048, 0x5A5A0000u + ring);
            dw = VCN45_WRITE_MEMORY_DW;
         } else {
            for (uint32_t i = 0; i < dw; i++) m[i] = 0x80000000u;
         }
         struct drm_amdgpu_cs_chunk_ib ib = {.ip_type = eng[e].ip, .ring = ring, .va_start = va, .ib_bytes = dw * 4};
         struct drm_amdgpu_cs_chunk ch = {.chunk_id = AMDGPU_CHUNK_ID_IB, .length_dw = sizeof ib / 4, .chunk_data = (uintptr_t)&ib};
         uint64_t chp = (uintptr_t)&ch;
         union drm_amdgpu_cs cs; memset(&cs, 0, sizeof cs);
         cs.in.ctx_id = ctx.out.alloc.ctx_id; cs.in.num_chunks = 1; cs.in.chunks = (uintptr_t)&chp;
         const int rc = acd_ioctl(d, DRM_AMDGPU_CS, &cs, sizeof cs);
         CHECK(!rc, "%s ring %u (advertised in 0x%x) refused: CS -> %d", eng[e].name, ring, ip.available_rings, rc);
      }
      printf("acd_bench: %s rings 0x%x\n", eng[e].name, ip.available_rings);
   }

   if (vcn) {
      for (uint32_t j = 0; j < 4; j++) {
         const uint32_t slot = 1024 + j, value = 0xC0DE0000u + j;
         m[slot] = 0;
         vcn45_write_memory_ib(m, va + 4ull * slot, value);
         struct drm_amdgpu_cs_chunk_ib ib = {.ip_type = AMDGPU_HW_IP_VCN_ENC, .va_start = va, .ib_bytes = VCN45_WRITE_MEMORY_DW * 4};
         struct drm_amdgpu_cs_chunk ch = {.chunk_id = AMDGPU_CHUNK_ID_IB, .length_dw = sizeof ib / 4, .chunk_data = (uintptr_t)&ib};
         uint64_t chp = (uintptr_t)&ch;
         union drm_amdgpu_cs cs; memset(&cs, 0, sizeof cs);
         cs.in.ctx_id = ctx.out.alloc.ctx_id; cs.in.num_chunks = 1; cs.in.chunks = (uintptr_t)&chp;
         const int rc = acd_ioctl(d, DRM_AMDGPU_CS, &cs, sizeof cs);
         CHECK(!rc && cs.out.handle, "CS %u -> %d (seq %llu)", j, rc, (unsigned long long)cs.out.handle);
         CHECK(m[slot] == value, "VCN job %u: 0x%08x != 0x%08x", j, m[slot], value);
      }
   }
   acd_bo_free(d, bo);

   /* two threads allocating and freeing at once, as the Metal plugin's main and pump threads do (round 56: "kext
    * returned BO handle 28 twice", then "Failed to allocate a buffer"): a handle the kext has freed must already be
    * out of the backend's table when another thread's allocation gets it back */
   {
      struct stress { acd_device_handle d; int fails; } st[2] = {{d, 0}, {d, 0}};
      pthread_t th[2];
      for (int t = 0; t < 2; t++) pthread_create(&th[t], NULL, stress_thread, &st[t]);
      for (int t = 0; t < 2; t++) pthread_join(th[t], NULL);
      CHECK(st[0].fails + st[1].fails == 0, "BO alloc/free from 2 threads: %d of %d allocations failed",
            st[0].fails + st[1].fails, 2 * STRESS_N);
      printf("acd_bench: 2 threads x %d BO alloc/free, %d failed\n", STRESS_N, st[0].fails + st[1].fails);
   }
   acd_device_deinitialize(d);
   printf("acd_bench: %s\n", fails ? "FAILED" : "PASS");
   return fails != 0;
}
