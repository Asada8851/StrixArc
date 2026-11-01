/* vkshim: a stand-in for the Vulkan loader with exactly one driver, StrixArc's RADV, for programs that dlopen the loader
 * and take vkGetInstanceProcAddr from it (FFmpeg's hwcontext_vulkan does). The driver's entry point
 * vk_icdGetInstanceProcAddr answers every query, global (instance NULL) ones included; no layers, no other ICDs, no
 * dispatch tables of its own (the plugin drives RADV the same way). The driver: $STRIXARC_RADV, else the default below.
 * Built by tools/cross/build-ffmpeg.sh as lib/libvulkan.dylib only (the name FFmpeg dlopens first; not libvulkan.1.dylib,
 * the name programs link the real loader under). For the Linux bench: cc -O2 -shared -fPIC -o libvulkan.so.1 vkshim.c -ldl */
#define _GNU_SOURCE /* dladdr on Linux */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifdef __APPLE__
#define DEFAULT_RADV "/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib"
#else
#define DEFAULT_RADV "/usr/lib/libvulkan_radeon.so"
#endif

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
static PFN_vkGetInstanceProcAddr icd_gipa;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
   const char *path = getenv("STRIXARC_RADV");
   if (!path || !*path) path = DEFAULT_RADV;
   void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "vkshim: dlopen %s: %s\n", path, dlerror()); return; }
   PFN_negotiate neg = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
   if (neg) { uint32_t v = 5; neg(&v); }
   icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!icd_gipa) { fprintf(stderr, "vkshim: %s has no vk_icdGetInstanceProcAddr\n", path); return; }
   /* the file the entry point really lives in: dyld may hand back an image already loaded under the same install name
    * (in mpv the Metal plugin has loaded the installed RADV by then) */
   Dl_info di;
   if (getenv("VKSHIM_VERBOSE"))
      fprintf(stderr, "vkshim: Vulkan from %s\n", dladdr((void *)icd_gipa, &di) && di.dli_fname ? di.dli_fname : path);
}

/* without the driver: what the loader answers with no driver at all (programs call the global commands unchecked;
 * FFmpeg crashed on NULL) */
static VKAPI_ATTR VkResult VKAPI_CALL none_enum_ext(const char *layer, uint32_t *n, VkExtensionProperties *p)
{
   (void)layer; (void)p; *n = 0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL none_enum_layers(uint32_t *n, VkLayerProperties *p)
{
   (void)p; *n = 0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL none_version(uint32_t *v)
{
   *v = VK_API_VERSION_1_0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL none_create(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                  VkInstance *i)
{
   (void)ci; (void)a; (void)i; return VK_ERROR_INCOMPATIBLE_DRIVER;
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
   pthread_once(&once, load);
   if (icd_gipa) return icd_gipa(instance, name);
   if (instance || !name) return NULL;
   if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
   if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)none_enum_ext;
   if (!strcmp(name, "vkEnumerateInstanceLayerProperties")) return (PFN_vkVoidFunction)none_enum_layers;
   if (!strcmp(name, "vkEnumerateInstanceVersion")) return (PFN_vkVoidFunction)none_version;
   if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)none_create;
   return NULL;
}
