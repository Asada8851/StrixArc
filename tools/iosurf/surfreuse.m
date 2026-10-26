// IOSurface backings kept while the surface lives: surfaces made and dropped in a loop (their IDs come back), each filled
// by the CPU with its own value and wrapped twice (the second wrapper after the first is gone, as CoreAnimation's flatten
// cache does), then read by the GPU through a blit. Every read must see that surface's value, never a previous one's.
// Usage: surfreuse [rounds]
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const int rounds = argc > 1 ? atoi(argv[1]) : 200;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int W = 256, H = 64;
        id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        int bad = 0, reused = 0; uint32_t lastID = 0;
        for (int r = 0; r < rounds; r++) @autoreleasepool {
            IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
                (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @('BGRA') });
            if (IOSurfaceGetID(s) == lastID) reused++;
            lastID = IOSurfaceGetID(s);
            const uint32_t v = 0xff000000u | (uint32_t)(r * 2654435761u >> 8);
            IOSurfaceLock(s, 0, NULL);
            for (int y = 0; y < H; y++) { uint32_t *row = (uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s));
                for (int x = 0; x < W; x++) row[x] = v; }
            IOSurfaceUnlock(s, 0, NULL);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
            for (int pass = 0; pass < 2; pass++) {
                id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
                memset(out.contents, 0, W * H * 4);
                id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                           toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
                [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
                const uint32_t *o = out.contents;
                if (o[0] != v || o[W * H - 1] != v) { if (bad < 5) printf("round %d pass %d: read 0x%08x/0x%08x, want 0x%08x\n", r, pass, o[0], o[W * H - 1], v); bad++; }
            }
            CFRelease(s);
        }
        printf("%d rounds, %d surface IDs came straight back, %d wrong reads\n", rounds, reused, bad);
        return bad ? 1 : 0;
    }
}
