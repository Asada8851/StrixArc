// Function constants of several types (bool, int, short, half, float) specialising a compute kernel, as CoreImage's
// precompiled filter kernels are specialised: each constant steers a value the kernel writes.
// Usage: fcprobe2
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"constant bool cb [[function_constant(0)]];\n"
"constant int ci [[function_constant(1)]];\n"
"constant short cs [[function_constant(2)]];\n"
"constant half ch [[function_constant(3)]];\n"
"constant float cf [[function_constant(4)]];\n"
"constant bool has_ci = is_function_constant_defined(ci);\n"
"kernel void k(device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
"  if (i != 0) return;\n"
"  out[0] = cb ? 1.0 : -1.0; out[1] = has_ci ? float(ci) : -99.0; out[2] = float(cs); out[3] = float(ch); out[4] = cf;\n"
"  float acc = 0; for (int k = 0; k < (has_ci ? ci : 0); k++) acc += 1.0; out[5] = acc;\n"
"}\n";

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        for (int round = 0; round < 2; round++) {
            bool b = round == 0; int iv = round ? 7 : 3; short sv = round ? -5 : 9; __fp16 hv = round ? 0.25 : 1.5; float fv = round ? 2.5f : -4.0f;
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            [cv setConstantValue:&b type:MTLDataTypeBool atIndex:0];
            [cv setConstantValue:&iv type:MTLDataTypeInt atIndex:1];
            [cv setConstantValue:&sv type:MTLDataTypeShort atIndex:2];
            [cv setConstantValue:&hv type:MTLDataTypeHalf atIndex:3];
            [cv setConstantValue:&fv type:MTLDataTypeFloat atIndex:4];
            id<MTLFunction> f = [lib newFunctionWithName:@"k" constantValues:cv error:&e];
            id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
            if (!ps) { printf("round %d: no pipeline (%s)\n", round, e.localizedDescription.UTF8String); bad++; continue; }
            id<MTLBuffer> out = [dev newBufferWithLength:6 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cbuf = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cbuf computeCommandEncoder];
            [en setComputePipelineState:ps]; [en setBuffer:out offset:0 atIndex:0];
            [en dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [en endEncoding];
            [cbuf commit]; [cbuf waitUntilCompleted];
            const float *o = out.contents;
            const float want[6] = { b ? 1.f : -1.f, (float)iv, (float)sv, (float)hv, fv, (float)iv };
            int rb = 0; for (int i = 0; i < 6; i++) if (o[i] != want[i]) rb++;
            printf("round %d: bool %4.1f int %5.1f short %5.1f half %5.2f float %5.2f loop %4.1f  %s\n", round, o[0], o[1], o[2], o[3], o[4], o[5], rb ? "WRONG" : "ok");
            bad += rb;
        }
        return bad ? 1 : 0;
    }
}
