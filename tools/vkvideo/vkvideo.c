/* vkvideo: does RADV offer Vulkan Video decode on this GPU, and does a decode session come up? No loader: the driver
 * is opened directly and its entry points come from vk_icdGetInstanceProcAddr, as the Metal plugin uses RADV. Checks
 * the video decode queue family and its codecs, the VK_KHR_video_* extensions, the decode capabilities of H.264 High
 * and H.265 Main / Main 10, and creates an H.264 1920x1080 video session with its memory bound, then tears it down.
 * Runs on macOS (StrixArc's RADV, needs the kext's VCN) and on Linux against the system RADV - the reference.
 * Build (macOS): clang -O2 -o vkvideo vkvideo.c          (Linux): cc -O2 -o vkvideo vkvideo.c -ldl
 *   vkvideo [path to libvulkan_radeon]      exit 0: a decode queue with H.264 and H.265, and the session came up */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>

#ifdef __APPLE__
#define DEFAULT_ICD "/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib"
#else
#define DEFAULT_ICD "/usr/lib/libvulkan_radeon.so"
#endif

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *ops(VkVideoCodecOperationFlagsKHR f)
{
   static char s[128]; s[0] = 0;
   if (f & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) strcat(s, "decode-h264 ");
   if (f & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) strcat(s, "decode-h265 ");
   if (f & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) strcat(s, "decode-av1 ");
   if (f & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) strcat(s, "encode-h264 ");
   if (f & VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) strcat(s, "encode-h265 ");
   return s[0] ? s : "none";
}

int main(int argc, char **argv)
{
   const char *icd = argc > 1 ? argv[1] : DEFAULT_ICD;
   void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!h) { printf("FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
   PFN_negotiate neg = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("FAIL: %s has no vk_icdGetInstanceProcAddr\n", icd); return 1; }
   if (neg) { uint32_t v = 5; neg(&v); }

   IFN(vkCreateInstance);
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkvideo", .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL: vkCreateInstance\n"); return 1; }
   IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties2);
   IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceVideoCapabilitiesKHR); IFN(vkCreateDevice);
   IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
   uint32_t n = 1; VkPhysicalDevice pd;
   if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL: no physical device\n"); return 1; }
   VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
   printf("vkvideo: %s (%s), driver 0x%x\n", pp.deviceName, icd, pp.driverVersion);

   /* queue families: which one decodes, and what */
   uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qn, NULL);
   VkQueueFamilyProperties2 qf[16]; VkQueueFamilyVideoPropertiesKHR qv[16];
   if (qn > 16) qn = 16;
   for (uint32_t i = 0; i < qn; i++) {
      qv[i] = (VkQueueFamilyVideoPropertiesKHR){.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR};
      qf[i] = (VkQueueFamilyProperties2){.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, .pNext = &qv[i]};
   }
   vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qn, qf);
   int dec = -1;
   for (uint32_t i = 0; i < qn; i++) {
      const VkQueueFlags f = qf[i].queueFamilyProperties.queueFlags;
      printf("  queue family %u: flags 0x%x x%u%s%s, codecs %s\n", i, f, qf[i].queueFamilyProperties.queueCount,
             f & VK_QUEUE_VIDEO_DECODE_BIT_KHR ? " VIDEO_DECODE" : "", f & VK_QUEUE_VIDEO_ENCODE_BIT_KHR ? " VIDEO_ENCODE" : "",
             ops(qv[i].videoCodecOperations));
      if ((f & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && dec < 0) dec = (int)i;
   }
   CHECK(dec >= 0, "no queue family with VIDEO_DECODE");
   if (dec >= 0) {
      CHECK(qv[dec].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "decode queue without H.264");
      CHECK(qv[dec].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "decode queue without H.265");
   }

   /* extensions */
   uint32_t en = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
   VkExtensionProperties *ex = calloc(en ? en : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(pd, NULL, &en, ex);
   static const char *want[] = {"VK_KHR_video_queue", "VK_KHR_video_decode_queue", "VK_KHR_video_decode_h264", "VK_KHR_video_decode_h265"};
   for (unsigned w = 0; w < 4; w++) {
      int have = 0;
      for (uint32_t i = 0; i < en; i++) have |= !strcmp(ex[i].extensionName, want[w]);
      printf("  %-28s %s\n", want[w], have ? "yes" : "NO");
      CHECK(have, "no %s", want[w]);
   }

   /* decode capabilities per profile */
   VkVideoDecodeH264ProfileInfoKHR h264 = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR,
      .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH, .pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR};
   VkVideoDecodeH265ProfileInfoKHR h265 = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR, .stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN};
   VkVideoDecodeH265ProfileInfoKHR h265m10 = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR, .stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10};
   struct { const char *name; void *codec; VkVideoCodecOperationFlagBitsKHR op; VkVideoComponentBitDepthFlagsKHR depth; } prof[] = {
      {"H.264 High 8-bit", &h264, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
      {"H.265 Main 8-bit", &h265, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
      {"H.265 Main10", &h265m10, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR},
   };
   VkVideoProfileInfoKHR vp[3];
   for (unsigned p = 0; p < 3; p++) {
      vp[p] = (VkVideoProfileInfoKHR){.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, .pNext = prof[p].codec,
         .videoCodecOperation = prof[p].op, .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
         .lumaBitDepth = prof[p].depth, .chromaBitDepth = prof[p].depth};
      VkVideoDecodeH264CapabilitiesKHR c264 = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
      VkVideoDecodeH265CapabilitiesKHR c265 = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
      VkVideoDecodeCapabilitiesKHR dc = {.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,
         .pNext = prof[p].op == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR ? (void *)&c264 : (void *)&c265};
      VkVideoCapabilitiesKHR caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &dc};
      VkResult r = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &vp[p], &caps);
      printf("  %-18s -> %d: max %ux%u, dpb %u slots %u refs, flags 0x%x\n", prof[p].name, r, caps.maxCodedExtent.width,
             caps.maxCodedExtent.height, caps.maxDpbSlots, caps.maxActiveReferencePictures, dc.flags);
      CHECK(r == VK_SUCCESS && caps.maxCodedExtent.width >= 3840 && caps.maxCodedExtent.height >= 2160, "%s capabilities", prof[p].name);
   }

   /* a decode session: device with the decode queue, an H.264 1080p session, its memory bound */
   if (dec >= 0) {
      const float prio = 1.0f;
      VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)dec,
                                     .queueCount = 1, .pQueuePriorities = &prio};
      const char *dext[] = {"VK_KHR_video_queue", "VK_KHR_video_decode_queue", "VK_KHR_video_decode_h264", "VK_KHR_video_decode_h265"};
      VkPhysicalDeviceSynchronization2Features s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .synchronization2 = VK_TRUE};
      VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &s2, .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &qci, .enabledExtensionCount = 4, .ppEnabledExtensionNames = dext};
      VkDevice dev;
      VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
      CHECK(r == VK_SUCCESS, "vkCreateDevice with the decode queue -> %d", r);
      if (r == VK_SUCCESS) {
         PFN_vkCreateVideoSessionKHR createS = (PFN_vkCreateVideoSessionKHR)vkGetDeviceProcAddr(dev, "vkCreateVideoSessionKHR");
         PFN_vkGetVideoSessionMemoryRequirementsKHR reqS = (PFN_vkGetVideoSessionMemoryRequirementsKHR)vkGetDeviceProcAddr(dev, "vkGetVideoSessionMemoryRequirementsKHR");
         PFN_vkBindVideoSessionMemoryKHR bindS = (PFN_vkBindVideoSessionMemoryKHR)vkGetDeviceProcAddr(dev, "vkBindVideoSessionMemoryKHR");
         PFN_vkDestroyVideoSessionKHR destroyS = (PFN_vkDestroyVideoSessionKHR)vkGetDeviceProcAddr(dev, "vkDestroyVideoSessionKHR");
         PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)vkGetDeviceProcAddr(dev, "vkAllocateMemory");
         PFN_vkFreeMemory freeM = (PFN_vkFreeMemory)vkGetDeviceProcAddr(dev, "vkFreeMemory");
         PFN_vkDestroyDevice destroyD = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
         PFN_vkGetDeviceQueue getQ = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
         VkQueue q = VK_NULL_HANDLE; getQ(dev, (uint32_t)dec, 0, &q);
         CHECK(q != VK_NULL_HANDLE, "no decode queue handle");
         VkExtensionProperties stdh = {.extensionName = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
                                       .specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION};
         VkVideoSessionCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, .queueFamilyIndex = (uint32_t)dec,
            .pVideoProfile = &vp[0], .pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, .maxCodedExtent = {1920, 1088},
            .referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, .maxDpbSlots = 17, .maxActiveReferencePictures = 16,
            .pStdHeaderVersion = &stdh};
         VkVideoSessionKHR vs;
         r = createS(dev, &sci, NULL, &vs);
         CHECK(r == VK_SUCCESS, "vkCreateVideoSessionKHR -> %d", r);
         if (r == VK_SUCCESS) {
            uint32_t mn = 0; reqS(dev, vs, &mn, NULL);
            VkVideoSessionMemoryRequirementsKHR mr[16];
            if (mn > 16) mn = 16;
            for (uint32_t i = 0; i < mn; i++) mr[i] = (VkVideoSessionMemoryRequirementsKHR){.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR};
            reqS(dev, vs, &mn, mr);
            VkDeviceMemory mem[16]; VkBindVideoSessionMemoryInfoKHR bi[16]; uint64_t total = 0;
            for (uint32_t i = 0; i < mn; i++) {
               uint32_t bits = mr[i].memoryRequirements.memoryTypeBits, t = 0;
               while (t < 32 && !(bits & (1u << t))) t++;
               VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr[i].memoryRequirements.size, .memoryTypeIndex = t};
               CHECK(alloc(dev, &ai, NULL, &mem[i]) == VK_SUCCESS, "session memory %u (%llu bytes)", i, (unsigned long long)ai.allocationSize);
               bi[i] = (VkBindVideoSessionMemoryInfoKHR){.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
                  .memoryBindIndex = mr[i].memoryBindIndex, .memory = mem[i], .memoryOffset = 0, .memorySize = ai.allocationSize};
               total += ai.allocationSize;
            }
            r = mn ? bindS(dev, vs, mn, bi) : VK_SUCCESS;
            CHECK(r == VK_SUCCESS, "vkBindVideoSessionMemoryKHR -> %d", r);
            printf("  H.264 1920x1088 session: %u memory binds, %llu KiB, bind %d\n", mn, (unsigned long long)(total >> 10), r);
            destroyS(dev, vs, NULL);
            for (uint32_t i = 0; i < mn; i++) freeM(dev, mem[i], NULL);
         }
         destroyD(dev, NULL);
      }
   }
   vkDestroyInstance(inst, NULL);
   printf("vkvideo: %s\n", fails ? "FAIL" : "PASS");
   return fails != 0;
}
