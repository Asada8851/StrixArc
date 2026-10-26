// Step by step through CoreImage's pattern: a kernel writes an IOSurface through wrapper A; the surface is checked
// (A's getBytes, the surface memory); then a kernel reads it through a new wrapper B in another command buffer.
// Usage: chainprobe [width height [mode]]   mode one: both passes in one command buffer, A released before B is made;
//        keep: one command buffer, A kept alive, B new; same: one command buffer, both passes through A
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void fill(texture2d<half, access::write> d [[texture(1)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= d.get_width() || g.y >= d.get_height()) return;\n"
"  d.write(half4(g.x < d.get_width() / 2 ? 0.0h : 1.0h, 0.5h, 1.0h, 1.0h), g);\n"
"}\n"
"kernel void noop(texture2d<half, access::write> d [[texture(OUTIDX)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x > 100000) d.write(half4(0.25h), g);\n"
"}\n"
"kernel void copy(texture2d<half, access::sample> s [[texture(0)]], texture2d<half, access::write> d [[texture(OUTIDX)]],\n"
"                 sampler sam [[sampler(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= d.get_width() || g.y >= d.get_height()) return;\n"
"  d.write(s.sample(sam, float2(g) + 0.5), g);\n"
"}\n";

int main(int argc, char **argv)
{
    @autoreleasepool {
        const int W = argc > 2 ? atoi(argv[1]) : 64, H = argc > 2 ? atoi(argv[2]) : 8, one = argc > 3, keep = argc > 3 && !strcmp(argv[3], "keep"), same = argc > 3 && !strcmp(argv[3], "same");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        // OUT2=1: the second kernel writes texture(2), so the two passes do not share a storage slot
        const int outIdx = getenv("OUT2") ? 2 : 1;
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:[src stringByReplacingOccurrencesOfString:@"OUTIDX" withString:@(outIdx).stringValue] options:nil error:&e];
        id<MTLComputePipelineState> pf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&e];
        // NOOP=1: the second kernel declares the output but writes nothing - whatever lands in out came from the first
        id<MTLComputePipelineState> pc = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:getenv("NOOP") ? @"noop" : @"copy"] error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.normalizedCoordinates = NO;
        id<MTLSamplerState> sam = [dev newSamplerStateWithDescriptor:sd];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
            (id)kIOSurfaceBytesPerElement: @8, (id)kIOSurfacePixelFormat: @('RGhA') });
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
        od.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModeManaged;
        id<MTLTexture> out = [dev newTextureWithDescriptor:od];
        __fp16 *px = calloc(W * H * 4, 2);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLTexture> keptA = nil;
        @autoreleasepool {
            id<MTLTexture> a = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
            if (keep || same) keptA = a;
            id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
            [en setComputePipelineState:pf]; [en setTexture:a atIndex:1];
            [en dispatchThreadgroups:MTLSizeMake((W + 15) / 16, (H + 3) / 4, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)]; [en endEncoding];
            if (!one) {
                [cb commit]; [cb waitUntilCompleted];
                [a getBytes:px bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                printf("1. fill through A: A.getBytes x=%d r %.2f g %.2f\n", W - 2, (float)px[((H / 2) * W + W - 2) * 4], (float)px[((H / 2) * W + W - 2) * 4 + 1]);
                cb = [q commandBuffer];
            }
        }
        @autoreleasepool {
            id<MTLTexture> b = same ? keptA : [dev newTextureWithDescriptor:td iosurface:surf plane:0];
            id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
            [en setComputePipelineState:pc]; [en setTexture:b atIndex:0]; [en setTexture:out atIndex:outIdx]; [en setSamplerState:sam atIndex:0];
            [en dispatchThreadgroups:MTLSizeMake((W + 15) / 16, (H + 3) / 4, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)]; [en endEncoding];
        }
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        [out getBytes:px bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        const __fp16 *sp = (const __fp16 *)((const uint8_t *)IOSurfaceGetBaseAddress(surf) + (H / 2) * IOSurfaceGetBytesPerRow(surf));
        const float sr = sp[(W - 2) * 4];
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        const int ok = fabsf((float)px[((H / 2) * W + W - 2) * 4] - 1) < 0.01f && fabsf((float)px[((H / 2) * W + W - 2) * 4 + 1] - 0.5f) < 0.01f;
        if (getenv("NOOP")) printf("NOOP second pass: out x=%d r %.2f g %.2f b %.2f (the fill pattern here = the first dispatch wrote out)\n", W - 2,
                                   (float)px[((H / 2) * W + W - 2) * 4], (float)px[((H / 2) * W + W - 2) * 4 + 1], (float)px[((H / 2) * W + W - 2) * 4 + 2]);
        printf("%dx%d row %zu, %s: out x=%d r %.2f g %.2f, surface r %.2f  %s\n", W, H, IOSurfaceGetBytesPerRow(surf),
               same ? "one CB, A for both" : keep ? "one CB, A kept, B new" : one ? "one CB, A released, B new" : "two command buffers", W - 2, (float)px[((H / 2) * W + W - 2) * 4], (float)px[((H / 2) * W + W - 2) * 4 + 1], sr, ok ? "ok" : "WRONG");
        free(px);
    }
    return 0;
}
