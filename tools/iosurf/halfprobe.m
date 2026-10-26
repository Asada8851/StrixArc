// 16-bit data through buffers, as CoreImage's convolution kernels take their weights (constant half wt[5]): reads of
// half from constant and device buffers at odd element offsets, half arithmetic, and a short-struct parameter block.
// Usage: halfprobe
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"struct P { short2 a; short b; half c; float d; };\n"
"kernel void k(constant half *wt [[buffer(0)]], device const half *dw [[buffer(1)]], constant P &p [[buffer(2)]],\n"
"              device float *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
"  if (i >= 5) return;\n"
"  out[i] = float(wt[i]);\n"
"  out[5 + i] = float(dw[i]);\n"
"  half acc = 0; for (int k = 0; k < 5; k++) acc += wt[k] * half(k + 1);\n"
"  out[10 + i] = float(acc);\n"
"  out[15 + i] = i == 0 ? float(p.a.x) : i == 1 ? float(p.a.y) : i == 2 ? float(p.b) : i == 3 ? float(p.c) : p.d;\n"
"}\n";

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        __fp16 w[5] = { 0.5, 0.25, 0.125, 1.5, -2.0 };
        struct { int16_t ax, ay, b; __fp16 c; float d; } p = { 7, -3, 11, (__fp16)0.75, 42.5f };
        id<MTLBuffer> dw = [dev newBufferWithBytes:w length:sizeof w options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [dev newBufferWithLength:20 * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        [en setComputePipelineState:ps];
        [en setBytes:w length:sizeof w atIndex:0];
        [en setBuffer:dw offset:0 atIndex:1];
        [en setBytes:&p length:sizeof p atIndex:2];
        [en setBuffer:out offset:0 atIndex:3];
        [en dispatchThreads:MTLSizeMake(5, 1, 1) threadsPerThreadgroup:MTLSizeMake(5, 1, 1)];
        [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const float *o = out.contents;
        const float acc = 0.5f * 1 + 0.25f * 2 + 0.125f * 3 + 1.5f * 4 - 2.0f * 5;
        const float want[20] = { 0.5, 0.25, 0.125, 1.5, -2, 0.5, 0.25, 0.125, 1.5, -2, acc, acc, acc, acc, acc, 7, -3, 11, 0.75, 42.5 };
        const char *row[4] = { "constant half[5] (setBytes)", "device half[5]", "half accumulate", "short/half struct (setBytes)" };
        int bad = 0;
        for (int r = 0; r < 4; r++) {
            int rb = 0; printf("%-30s", row[r]);
            for (int i = 0; i < 5; i++) { printf(" %7.3f", o[r * 5 + i]); if (o[r * 5 + i] != want[r * 5 + i]) rb++; }
            printf("  %s\n", rb ? "WRONG" : "ok"); bad += rb;
        }
        return bad ? 1 : 0;
    }
}
