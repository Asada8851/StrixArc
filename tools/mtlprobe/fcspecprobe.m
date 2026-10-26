// How long a new specialization of one of CoreAnimation's compositor shaders takes: loads QuartzCore's default.metallib,
// gives a function (default fixed_frag_lpf_cpf, the fragment uber-shader WindowServer specializes per layer kind) a set of
// function constants never asked for before (bools from a seed, numbers zero), and times newFunctionWithName:
// constantValues: and the pipeline built from it. WindowServer blocked ~4 s on such a miss (2026-10-21).
// Usage: fcspecprobe [seed] [function] [vertex function]
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <time.h>

static double now(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        const unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : (unsigned)time(NULL);
        NSString *fname = argc > 2 ? @(argv[2]) : @"fixed_frag_lpf_cpf", *vname = argc > 3 ? @(argv[3]) : nil;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib"] error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLFunction> plain = [lib newFunctionWithName:fname];
        NSDictionary<NSString *, MTLFunctionConstant *> *fcs = plain.functionConstantsDictionary;
        printf("%s: %lu function constants\n", fname.UTF8String, (unsigned long)fcs.count);
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        unsigned r = seed; int nb = 0;
        for (MTLFunctionConstant *c in fcs.allValues) {
            uint8_t zero[16] = {0};
            if (c.type == MTLDataTypeBool) { r = r * 1103515245u + 12345u; bool b = (r >> 16) & 1; nb += b;
                [cv setConstantValue:&b type:MTLDataTypeBool atIndex:c.index]; }
            else [cv setConstantValue:zero type:c.type atIndex:c.index];
        }
        printf("seed %u: %d of the bools set\n", seed, nb);
        double t0 = now();
        id<MTLFunction> f = [lib newFunctionWithName:fname constantValues:cv error:&e];
        double t1 = now();
        printf("newFunctionWithName:constantValues: %.1f ms%s\n", t1 - t0, f ? "" : " (nil)");
        if (!f) { printf("  %s\n", e.localizedDescription.UTF8String); return 1; }
        // a pipeline with a plain vertex function from the same library
        id<MTLFunction> v = nil;
        if (vname) v = [lib newFunctionWithName:vname];
        if (!v) for (NSString *n in lib.functionNames) { id<MTLFunction> x = [lib newFunctionWithName:n];
            if (x.functionType == MTLFunctionTypeVertex && !x.functionConstantsDictionary.count) { v = x; break; } }
        if (!v) { printf("no vertex function without constants\n"); return 0; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = v; pd.fragmentFunction = f; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        double t2 = now();
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        printf("newRenderPipelineState (vertex %s): %.1f ms%s\n", v.name.UTF8String, now() - t2, ps ? "" : " (nil)");
        if (!ps) printf("  %s\n", e.localizedDescription.UTF8String);
        return 0;
    }
}
