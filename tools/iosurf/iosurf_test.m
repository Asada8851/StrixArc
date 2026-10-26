// Checks that a Metal texture made over an IOSurface shares the surface's pages both ways (the plugin's host-import
// path): what the GPU renders is in the surface for the CPU, and what the CPU writes is what the GPU reads.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

static double ms(uint64_t a, uint64_t b) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (b - a) * tb.numer / tb.denom / 1e6; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        const unsigned W = argc > 1 ? atoi(argv[1]) : 2560, H = argc > 2 ? atoi(argv[2]) : 1600;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("FAIL: no Metal device\n"); return 1; }
        printf("device: %s\n", dev.name.UTF8String);
        if (getenv("IOSURF_PAUSE")) { printf("pid %d paused\n", getpid()); fflush(stdout); sleep(atoi(getenv("IOSURF_PAUSE"))); }
        NSDictionary *p = @{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        printf("surface %u: %ux%u row %zu alloc %zu\n", IOSurfaceGetID(s), W, H, IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s));
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
        if (!t) { printf("FAIL: no texture over the surface\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        // 1: GPU -> surface
        for (int it = 0; it < 3; it++) {
            const double r = it == 1 ? 0 : 1, g = it == 1 ? 1 : 0;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, 0, 1);
            uint64_t t0 = mach_absolute_time();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            uint64_t t1 = mach_absolute_time();
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            const uint32_t *px = IOSurfaceGetBaseAddress(s); const size_t rw = IOSurfaceGetBytesPerRow(s) / 4;
            const uint32_t want = it == 1 ? 0xFF00FF00u : 0xFFFF0000u, a = px[0], b = px[(H - 1) * rw + W - 1];
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            printf("GPU clear #%d: %.2f ms, surface reads 0x%08x / 0x%08x (want 0x%08x) %s\n", it, ms(t0, t1), a, b, want, a == want && b == want ? "OK" : "WRONG");
            bad += !(a == want && b == want);
        }
        // 2: CPU -> GPU
        IOSurfaceLock(s, 0, NULL);
        uint32_t *px = IOSurfaceGetBaseAddress(s); const size_t rw = IOSurfaceGetBytesPerRow(s) / 4;
        for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) px[y * rw + x] = 0xFF0000FFu;   // blue
        IOSurfaceUnlock(s, 0, NULL);
        MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        pd.storageMode = MTLStorageModeManaged;
        id<MTLTexture> c = [dev newTextureWithDescriptor:pd];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t toTexture:c]; [bl synchronizeResource:c]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        uint32_t got[2] = {0, 0};
        [c getBytes:&got[0] bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        [c getBytes:&got[1] bytesPerRow:4 fromRegion:MTLRegionMake2D(W - 1, H - 1, 1, 1) mipmapLevel:0];
        printf("CPU write -> GPU copy: 0x%08x / 0x%08x (want 0xff0000ff) %s\n", got[0], got[1], got[0] == 0xFF0000FFu && got[1] == 0xFF0000FFu ? "OK" : "WRONG");
        bad += !(got[0] == 0xFF0000FFu && got[1] == 0xFF0000FFu);
        printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS");
        CFRelease(s);
        return bad != 0;
    }
}
