// The shape of CoreImage's k5x1 convolution kernel: a compute kernel samples its source with pixel coordinates and
// constant offsets (-2..2), weights them with half constants and writes the destination. The source is an ordinary
// texture or an IOSurface texture (RGBA16F, as CoreImage's intermediates), the destination likewise.
// Usage: convprobe
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void k5x1(texture2d<half, access::sample> s [[texture(0)]], texture2d<half, access::write> d [[texture(1)]],\n"
"                 sampler sam [[sampler(0)]], constant half *wt [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= d.get_width() || g.y >= d.get_height()) return;\n"
"  float2 c = float2(g) + 0.5;\n"
"  half4 a = wt[0] * s.sample(sam, c, int2(-2, 0)) + wt[1] * s.sample(sam, c, int2(-1, 0)) + wt[2] * s.sample(sam, c)\n"
"          + wt[3] * s.sample(sam, c, int2(1, 0)) + wt[4] * s.sample(sam, c, int2(2, 0));\n"
"  d.write(a, g);\n"
"}\n"
"kernel void k1(texture2d<half, access::sample> s [[texture(0)]], texture2d<half, access::write> d [[texture(1)]],\n"
"               sampler sam [[sampler(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= d.get_width() || g.y >= d.get_height()) return;\n"
"  d.write(s.sample(sam, float2(g) + 0.5), g);\n"
"}\n";

static id<MTLTexture> make(id<MTLDevice> dev, int w, int h, BOOL surf, MTLTextureUsage u)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
    td.usage = u;
    if (!surf) { td.storageMode = MTLStorageModeManaged; return [dev newTextureWithDescriptor:td]; }
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
        (id)kIOSurfaceBytesPerElement: @8, (id)kIOSurfacePixelFormat: @('RGhA') });
    td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td iosurface:s plane:0];
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.normalizedCoordinates = NO;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> sam = [dev newSamplerStateWithDescriptor:sd];
        const int W = 64, H = 4;
        __fp16 px[W * H * 4];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { __fp16 *p = px + (y * W + x) * 4; p[0] = x < 32 ? 0 : 1; p[1] = x < 32 ? 0 : 1; p[2] = 1; p[3] = 1; }
        __fp16 wt[5] = { 0.2, 0.2, 0.2, 0.2, 0.2 };
        int bad = 0;
        for (int kind = 0; kind < 2; kind++)
            for (int ss = 0; ss < 2; ss++) for (int ds = 0; ds < 2; ds++) {
                id<MTLTexture> s = make(dev, W, H, ss, MTLTextureUsageShaderRead);
                id<MTLTexture> d = make(dev, W, H, ds, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
                [s replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 8];
                id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:kind ? @"k5x1" : @"k1"] error:&e];
                id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
                [en setComputePipelineState:ps]; [en setTexture:s atIndex:0]; [en setTexture:d atIndex:1]; [en setSamplerState:sam atIndex:0];
                [en setBytes:wt length:sizeof wt atIndex:1];
                [en dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)]; [en endEncoding];
                if (!ds) { id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:d]; [bl endEncoding]; }
                [cb commit]; [cb waitUntilCompleted];
                __fp16 o[W * H * 4];
                [d getBytes:o bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 ];
                const float a = o[(2 * W + 8) * 4], b = o[(2 * W + 31) * 4], c = o[(2 * W + 32) * 4], al = o[(2 * W + 8) * 4 + 3];
                const float wb = kind ? 0.4f : 0, wc = kind ? 0.6f : 1;
                const int ok = fabsf(a) < 0.01f && fabsf(b - wb) < 0.02f && fabsf(c - wc) < 0.02f && fabsf(al - 1) < 0.01f;
                printf("%-5s src %-8s dst %-8s: x=8 %.3f x=31 %.3f x=32 %.3f alpha %.3f  %s\n", kind ? "k5x1" : "copy",
                       ss ? "IOSurface" : "texture", ds ? "IOSurface" : "texture", a, b, c, al, ok ? "ok" : "WRONG");
                bad += !ok;
            }
        // CoreImage's shape: one IOSurface, a new MTLTexture per pass, both passes in one command buffer
        for (int wi = 0; wi < 4; wi++) {
            const int CW = (wi & 1) ? 272 : 64, CH = 8, reuse = wi >= 2;   // reuse: pass 2 reads wrapper A itself
            IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(CW), (id)kIOSurfaceHeight: @(CH),
                (id)kIOSurfaceBytesPerElement: @8, (id)kIOSurfacePixelFormat: @('RGhA') });
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:CW height:CH mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            id<MTLTexture> src0 = make(dev, CW, CH, NO, MTLTextureUsageShaderRead);
            __fp16 *pp = calloc(CW * CH * 4, 2);
            for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) { __fp16 *p = pp + (y * CW + x) * 4; p[0] = p[1] = x < CW / 2 ? 0 : 1; p[2] = p[3] = 1; }
            [src0 replaceRegion:MTLRegionMake2D(0, 0, CW, CH) mipmapLevel:0 withBytes:pp bytesPerRow:CW * 8]; free(pp);
            id<MTLTexture> out = make(dev, CW, CH, NO, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLTexture> keepA = nil;
            const BOOL keepPs = getenv("KEEP_PS") != NULL;   // KEEP_PS=1: the pipelines outlive the command buffer
            static id<MTLComputePipelineState> pk1, pk5;
            if (keepPs && !pk1) { pk1 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k1"] error:&e];
                                  pk5 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k5x1"] error:&e]; }
            @autoreleasepool {
                id<MTLTexture> a = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
                keepA = a;
                id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
                [en setComputePipelineState:keepPs ? pk1 : [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k1"] error:&e]];
                [en setTexture:src0 atIndex:0]; [en setTexture:a atIndex:1]; [en setSamplerState:sam atIndex:0];
                [en dispatchThreads:MTLSizeMake(CW, CH, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)]; [en endEncoding];
            }
            @autoreleasepool {
                id<MTLTexture> b = reuse ? keepA : [dev newTextureWithDescriptor:td iosurface:surf plane:0];
                id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
                [en setComputePipelineState:keepPs ? pk5 : [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k5x1"] error:&e]];
                [en setTexture:b atIndex:0]; [en setTexture:out atIndex:1]; [en setSamplerState:sam atIndex:0];
                [en setBytes:wt length:sizeof wt atIndex:1];
                [en dispatchThreads:MTLSizeMake(CW, CH, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)]; [en endEncoding];
            }
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            __fp16 *o = calloc(CW * CH * 4, 2);
            [out getBytes:o bytesPerRow:CW * 8 fromRegion:MTLRegionMake2D(0, 0, CW, CH) mipmapLevel:0];
            const int m = CW / 2;
            const float a0 = o[(2 * CW + 8) * 4], b0 = o[(2 * CW + m - 1) * 4], c0 = o[(2 * CW + m) * 4], al = o[(2 * CW + 8) * 4 + 3];
            const int ok = fabsf(a0) < 0.01f && fabsf(b0 - 0.4f) < 0.02f && fabsf(c0 - 0.6f) < 0.02f && fabsf(al - 1) < 0.01f;
            IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
            const __fp16 *sp = (const __fp16 *)((const uint8_t *)IOSurfaceGetBaseAddress(surf) + 2 * IOSurfaceGetBytesPerRow(surf));
            const float s8 = sp[8 * 4 + 2], sm = sp[m * 4], sa = sp[m * 4 + 3];
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
            printf("chain %3dx%d %s (row %zu): x=8 %.3f x=%d %.3f x=%d %.3f alpha %.3f | surface after pass 1: b@8 %.2f r@%d %.2f a %.2f  %s\n", CW, CH,
                   reuse ? "same wrapper" : "new wrapper ", IOSurfaceGetBytesPerRow(surf), a0, m - 1, b0, m, c0, al, s8, m, sm, sa, ok ? "ok" : "WRONG");
            bad += !ok; free(o);
        }
        return bad ? 1 : 0;
    }
}
