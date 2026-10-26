// Calls PhxBringup's BO_IMPORT_HOST (selector 21) directly on plain malloc'd pages and on an IOSurface's pages.
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

static void try(io_connect_t c, const char *what, void *p, uint64_t size)
{
    uint64_t in[4] = {(uint64_t)(uintptr_t)p, size, 0, 0}, out[4] = {0};
    uint32_t n = 4;
    kern_return_t kr = IOConnectCallMethod(c, 21, in, 4, NULL, 0, out, &n, NULL, NULL);
    printf("%-22s %p %8llu bytes -> kr 0x%08x handle %llu", what, p, (unsigned long long)size, kr, (unsigned long long)out[0]);
    if (!kr && out[0]) {
        // GemVa MAP at a high VA, as RADV does after the import, then UNMAP
        struct { uint32_t handle, pad, op, flags; uint64_t va, off, size, tp; uint32_t so, ns; uint64_t fh; } v = {0};
        v.handle = (uint32_t)out[0]; v.op = 1; v.flags = 0xe; v.va = 0x7f0000000000ull; v.size = size;
        kern_return_t mk = IOConnectCallMethod(c, 5, NULL, 0, &v, sizeof v, NULL, NULL, NULL, NULL);
        printf(", GemVa MAP kr 0x%08x", mk);
        if (!mk) { v.op = 2; IOConnectCallMethod(c, 5, NULL, 0, &v, sizeof v, NULL, NULL, NULL, NULL); }
        uint64_t h = out[0]; IOConnectCallMethod(c, 4, &h, 1, NULL, 0, NULL, NULL, NULL, NULL);
    }
    printf("\n");
}

int main(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { printf("open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4] = {0}; uint32_t hn = 4;
    kern_return_t kr = IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL);
    printf("hello kr 0x%08x abi 0x%llx build %llu\n", kr, (unsigned long long)hout[0], (unsigned long long)hout[1]);
    void *m = NULL; posix_memalign(&m, 4096, 1 << 20); ((char *)m)[0] = 1;
    try(c, "malloc 1 MiB", m, 1 << 20);
    int W = 2560, H = 1600, B = 4; uint32_t f = 'BGRA';
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &W), h = CFNumberCreate(NULL, kCFNumberIntType, &H),
                b = CFNumberCreate(NULL, kCFNumberIntType, &B), pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &f);
    const void *k[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat}, *v[] = {w, h, b, pf};
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef sf = IOSurfaceCreate(d);
    void *base = IOSurfaceGetBaseAddress(sf);
    size_t alloc = IOSurfaceGetAllocSize(sf);
    try(c, "IOSurface (unlocked)", base, (alloc + 4095) & ~4095ull);
    IOSurfaceLock(sf, 0, NULL);
    try(c, "IOSurface (locked)", base, (alloc + 4095) & ~4095ull);
    IOSurfaceUnlock(sf, 0, NULL);
    try(c, "IOSurface first page", base, 4096);
    return 0;
}
