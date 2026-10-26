// mtlprobe: lists the Metal devices and runs one compute kernel on the 780M (PhxAccel + NVMTLDriver + RADV).
// The name is on NVMTLDriver's built-in allow list. Build: clang -fobjc-arc -framework Foundation -framework Metal
//   -framework IOSurface -framework CoreGraphics -o mtlprobe mtlprobe.m
//   mtlprobe [list | render | stress [seconds] [width height]]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdatomic.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void k(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
     "    out[i] = i * 0x9E3779B1u + 7u;\n"
     "}\n";

// Render test: one triangle over the lower-left half of a 256x256 target cleared to blue, read back through a blit.
static NSString *const kGfxSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 pos [[position]]; float4 col; };\n"
     "vertex V vs(uint vid [[vertex_id]]) {\n"
     "    const float2 p[3] = { float2(-1, -1), float2(1, -1), float2(-1, 1) };\n"
     "    V o; o.pos = float4(p[vid], 0, 1); o.col = float4(1, 0, 0, 1); return o;\n"
     "}\n"
     "fragment float4 fs(V in [[stage_in]]) { return in.col; }\n";

static int renderTest(id<MTLDevice> dev) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kGfxSrc options:nil error:&err];
    if (!lib) { printf("render library: %s\n", err.localizedDescription.UTF8String); return 12; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) { printf("render pipeline: %s\n", err.localizedDescription.UTF8String); return 13; }
    const NSUInteger W = 256, H = 256;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    memset(out.contents, 0x55, W * H * 4);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    printf("render command buffer status %ld error %s\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
    const uint32_t *px = out.contents;
    NSUInteger red = 0, blue = 0, other = 0;
    for (NSUInteger i = 0; i < W * H; i++) {
        if (px[i] == 0xFFFF0000u) red++; else if (px[i] == 0xFF0000FFu) blue++; else other++;
    }
    printf("pixels: red %lu blue %lu other %lu (expect ~32896 red, ~32640 blue); corners: TL 0x%08x BL 0x%08x BR 0x%08x\n",
           (unsigned long)red, (unsigned long)blue, (unsigned long)other, px[0], px[(H - 1) * W], px[H * W - 1]);
    return (red > 30000 && blue > 30000) ? 0 : 14;
}

// Stress: what WindowServer asks of the GPU, in one process, repeated for a while. Display-sized BGRA textures, blits,
// alpha-blended draws, compositing by reading and by sampling a texture, a compute blur, IOSurfaces written by the CPU,
// drawn from and into by the GPU and read back by the CPU (the app -> WindowServer path), three queues with command
// buffers in flight, and buffer/texture churn. Every result is checked on the CPU. Each step prints its name before it
// runs, so a hang leaves the step it hung in at the end of the line.
static NSString *const kStressSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void hashk(device uint *out [[buffer(0)]], constant uint &seed [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "    out[i] = i * 0x9E3779B1u + seed;\n"
     "}\n"
     "struct V { float4 pos [[position]]; };\n"
     "vertex V fullscreen(uint vid [[vertex_id]]) {\n"   // one triangle over the whole target
     "    float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2 - 1, 0, 1); return o;\n"
     "}\n"
     "fragment float4 solid(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n"
     "fragment float4 copyread(V in [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(in.pos.xy)); }\n"
     "fragment float4 copysample(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    return t.sample(s, in.pos.xy / float2(t.get_width(), t.get_height()));\n"
     "}\n"
     "kernel void blurh(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
     "                  uint2 g [[thread_position_in_grid]]) {\n"
     "    int w = src.get_width(), h = src.get_height();\n"
     "    if (int(g.x) >= w || int(g.y) >= h) return;\n"
     "    float4 a = 0;\n"
     "    for (int d = -2; d <= 2; d++) a += src.read(uint2(clamp(int(g.x) + d, 0, w - 1), g.y));\n"
     "    dst.write(a / 5.0, g);\n"
     "}\n";

static uint32_t pat(NSUInteger x, NSUInteger y) {   // BGRA8 as a little-endian word: A R G B
    return 0xFF000000u | (uint32_t)((x * 7 + y * 13) & 0xFF) << 16 | (uint32_t)((x ^ y) & 0xFF) << 8 | (uint32_t)((x + 3 * y) & 0xFF);
}

static double nowMs(void) { return CFAbsoluteTimeGetCurrent() * 1000.0; }

static id<MTLRenderPipelineState> stressPipe(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *frag, BOOL blend) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"fullscreen"];
    pd.fragmentFunction = [lib newFunctionWithName:frag];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blend) {   // source-over, as WindowServer composites a translucent layer
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) printf("\nstress: pipeline %s: %s\n", frag.UTF8String, err.localizedDescription.UTF8String);
    return ps;
}

static void drawFull(id<MTLCommandBuffer> cb, id<MTLTexture> dst, MTLLoadAction load, MTLClearColor clear,
                     id<MTLRenderPipelineState> ps, id<MTLTexture> src, id<MTLSamplerState> smp, const float *color) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].clearColor = clear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:ps];
    if (src) [re setFragmentTexture:src atIndex:0];
    if (smp) [re setFragmentSamplerState:smp atIndex:0];
    if (color) [re setFragmentBytes:color length:16 atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
}

static void blitToBuffer(id<MTLCommandBuffer> cb, id<MTLTexture> t, id<MTLBuffer> b) {
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0
 destinationBytesPerRow:t.width * 4 destinationBytesPerImage:t.width * t.height * 4];
    [be endEncoding];
}

// "" when the command buffer completed, else why not
static const char *cbDone(id<MTLCommandBuffer> cb) {
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusCompleted && !cb.error) return "";
    static char why[256];
    snprintf(why, sizeof why, "command buffer status %ld error %s", (long)cb.status,
             cb.error ? cb.error.localizedDescription.UTF8String : "none");
    return why;
}

static int near8(uint32_t px, int sh, int want, int tol) { int v = (px >> sh) & 0xFF; return v >= want - tol && v <= want + tol; }

static int stressTest(id<MTLDevice> dev, double seconds, NSUInteger W, NSUInteger H) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kStressSrc options:nil error:&err];
    if (!lib) { printf("stress: library: %s\n", err.localizedDescription.UTF8String); return 20; }
    id<MTLComputePipelineState> hashPs = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"hashk"] error:&err];
    id<MTLComputePipelineState> blurPs = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"blurh"] error:&err];
    id<MTLRenderPipelineState> solidPs = stressPipe(dev, lib, @"solid", YES);
    id<MTLRenderPipelineState> readPs = stressPipe(dev, lib, @"copyread", NO);
    id<MTLRenderPipelineState> samplePs = stressPipe(dev, lib, @"copysample", NO);
    if (!hashPs || !blurPs || !solidPs || !readPs || !samplePs) { printf("stress: pipelines did not build\n"); return 21; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> nearest = [dev newSamplerStateWithDescriptor:sd];

    const NSUInteger N = W * H;
    uint32_t *ref = malloc(N * 4);
    for (NSUInteger y = 0; y < H; y++) for (NSUInteger x = 0; x < W; x++) ref[y * W + x] = pat(x, y);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td], dst = [dev newTextureWithDescriptor:td], tmp = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> up = [dev newBufferWithBytes:ref length:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> down = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    NSDictionary *sp = @{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
                          (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    IOSurfaceRef sa = IOSurfaceCreate((__bridge CFDictionaryRef)sp), sb = IOSurfaceCreate((__bridge CFDictionaryRef)sp);
    MTLTextureDescriptor *std_ = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    std_.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> ta = sa ? [dev newTextureWithDescriptor:std_ iosurface:sa plane:0] : nil;
    id<MTLTexture> tb = sb ? [dev newTextureWithDescriptor:std_ iosurface:sb plane:0] : nil;
    if (!src || !dst || !tmp || !up || !down || !ta || !tb) {
        printf("stress: allocation failed (textures %d%d%d, buffers %d%d, IOSurface textures %d%d)\n", !!src, !!dst, !!tmp,
               !!up, !!down, !!ta, !!tb);
        return 22;
    }
    printf("stress: %lux%lu BGRA8, %.0f s, device %s\n", (unsigned long)W, (unsigned long)H, seconds, dev.name.UTF8String);
    fflush(stdout);

    const char *fail = NULL; char why[512] = "";
    int it = 0, rc = 0;
    double t0 = nowMs(), stepMs[8] = {0};
    static const char *names[8] = {"compute", "blit", "blend", "composite", "blur", "iosurface", "inflight", "churn"};
#define STEP(i) do { s = i; printf(" %s", names[i]); fflush(stdout); ts = nowMs(); } while (0)
#define DONE() do { double d = nowMs() - ts; if (d > stepMs[s]) stepMs[s] = d; } while (0)
#define FAIL(...) do { snprintf(why, sizeof why, __VA_ARGS__); fail = names[s]; rc = 30 + s; goto out; } while (0)
    while (it == 0 || nowMs() - t0 < seconds * 1000.0) {
        @autoreleasepool {
        int s = 0; double ts = 0, ti = nowMs();
        it++;
        printf("stress[%d]:", it); fflush(stdout);
        const char *e;

        STEP(0);   // compute over 1M words
        {
            const NSUInteger n = 1 << 20; const uint32_t seed = 7u + (uint32_t)it;
            id<MTLBuffer> b = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
            memset(b.contents, 0, n * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:hashPs]; [ce setBuffer:b offset:0 atIndex:0]; [ce setBytes:&seed length:4 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding]; [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            const uint32_t *o = b.contents; NSUInteger bad = 0;
            for (NSUInteger i = 0; i < n; i++) bad += o[i] != (uint32_t)(i * 0x9E3779B1u + seed);
            if (bad) FAIL("%lu of %lu words wrong (out[1]=0x%08x)", (unsigned long)bad, (unsigned long)n, o[1]);
        }
        DONE();

        STEP(1);   // buffer -> texture -> buffer at display size
        {
            memset(down.contents, 0, N * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:N * 4 sourceSize:MTLSizeMake(W, H, 1)
                     toTexture:src destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            blitToBuffer(cb, src, down);
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            if (memcmp(down.contents, ref, N * 4)) {
                const uint32_t *o = down.contents; NSUInteger bad = 0, first = N;
                for (NSUInteger i = 0; i < N; i++) if (o[i] != ref[i]) { bad++; if (first == N) first = i; }
                FAIL("%lu of %lu pixels differ, first at (%lu,%lu): 0x%08x want 0x%08x", (unsigned long)bad, (unsigned long)N,
                     (unsigned long)(first % W), (unsigned long)(first / W), o[first], ref[first]);
            }
        }
        DONE();

        STEP(2);   // blue, then red at 50 %, then green at 50 %, source-over
        {
            const float red[4] = {1, 0, 0, 0.5f}, green[4] = {0, 1, 0, 0.5f};
            id<MTLCommandBuffer> cb = [q commandBuffer];
            drawFull(cb, dst, MTLLoadActionClear, MTLClearColorMake(0, 0, 1, 1), solidPs, nil, nil, red);
            drawFull(cb, dst, MTLLoadActionLoad, MTLClearColorMake(0, 0, 0, 0), solidPs, nil, nil, green);
            blitToBuffer(cb, dst, down);
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            const uint32_t *o = down.contents; NSUInteger bad = 0, first = N;
            for (NSUInteger i = 0; i < N; i++)   // (0.25, 0.5, 0.25, 1)
                if (!(near8(o[i], 16, 64, 2) && near8(o[i], 8, 128, 2) && near8(o[i], 0, 64, 2) && near8(o[i], 24, 255, 1))) {
                    bad++; if (first == N) first = i;
                }
            if (bad) FAIL("%lu of %lu pixels off (want ~0xff408040), first at (%lu,%lu): 0x%08x", (unsigned long)bad,
                          (unsigned long)N, (unsigned long)(first % W), (unsigned long)(first / W), o[first]);
        }
        DONE();

        STEP(3);   // composite: draw src into dst by read(), into tmp by sample(); both must equal src exactly
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            drawFull(cb, dst, MTLLoadActionDontCare, MTLClearColorMake(0, 0, 0, 0), readPs, src, nil, NULL);
            drawFull(cb, tmp, MTLLoadActionDontCare, MTLClearColorMake(0, 0, 0, 0), samplePs, src, nearest, NULL);
            blitToBuffer(cb, dst, down);
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            if (memcmp(down.contents, ref, N * 4)) FAIL("texture read() copy differs from the source");
            cb = [q commandBuffer];
            blitToBuffer(cb, tmp, down);
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            if (memcmp(down.contents, ref, N * 4)) {
                const uint32_t *o = down.contents; NSUInteger first = 0;
                while (first < N && o[first] == ref[first]) first++;
                FAIL("sampled copy differs from the source, first at (%lu,%lu): 0x%08x want 0x%08x",
                     (unsigned long)(first % W), (unsigned long)(first / W), o[first], ref[first]);
            }
        }
        DONE();

        STEP(4);   // compute: 5-tap horizontal box blur, src -> tmp, checked against the CPU
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:blurPs]; [ce setTexture:src atIndex:0]; [ce setTexture:tmp atIndex:1];
            [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [ce endEncoding];
            blitToBuffer(cb, tmp, down);
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            const uint32_t *o = down.contents; NSUInteger bad = 0, first = N; uint32_t want0 = 0;
            for (NSUInteger y = 0; y < H; y++) for (NSUInteger x = 0; x < W; x++) {
                uint32_t want = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int a = 0;
                    for (int d = -2; d <= 2; d++) {
                        long xx = (long)x + d; xx = xx < 0 ? 0 : xx >= (long)W ? (long)W - 1 : xx;
                        a += (ref[y * W + xx] >> sh) & 0xFF;
                    }
                    want |= (uint32_t)((a + 2) / 5) << sh;
                }
                const uint32_t got = o[y * W + x];
                int ok = 1;
                for (int sh = 0; sh < 32; sh += 8) ok &= near8(got, sh, (want >> sh) & 0xFF, 1);
                if (!ok) { bad++; if (first == N) { first = y * W + x; want0 = want; } }
            }
            if (bad) FAIL("%lu of %lu pixels off by more than 1, first at (%lu,%lu): 0x%08x want 0x%08x", (unsigned long)bad,
                          (unsigned long)N, (unsigned long)(first % W), (unsigned long)(first / W), o[first], want0);
        }
        DONE();

        STEP(5);   // IOSurface: CPU writes A, the GPU composites A into B, the CPU reads B
        {
            IOSurfaceLock(sa, 0, NULL);
            uint8_t *pa = IOSurfaceGetBaseAddress(sa); const size_t ra = IOSurfaceGetBytesPerRow(sa);
            for (NSUInteger y = 0; y < H; y++) memcpy(pa + y * ra, ref + y * W, W * 4);
            IOSurfaceUnlock(sa, 0, NULL);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            drawFull(cb, tb, MTLLoadActionClear, MTLClearColorMake(0, 1, 0, 1), readPs, ta, nil, NULL);
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];   // B is managed: what the Metal rules ask before a CPU read
            [be synchronizeResource:tb];
            [be endEncoding];
            [cb commit];
            if (*(e = cbDone(cb))) FAIL("%s", e);
            IOSurfaceLock(sb, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *pb = IOSurfaceGetBaseAddress(sb); const size_t rb = IOSurfaceGetBytesPerRow(sb);
            NSUInteger bad = 0, first = N; uint32_t got0 = 0;
            for (NSUInteger y = 0; y < H; y++) {
                const uint32_t *row = (const uint32_t *)(pb + y * rb);
                for (NSUInteger x = 0; x < W; x++) if (row[x] != ref[y * W + x]) { bad++; if (first == N) { first = y * W + x; got0 = row[x]; } }
            }
            IOSurfaceUnlock(sb, kIOSurfaceLockReadOnly, NULL);
            if (bad) FAIL("%lu of %lu IOSurface pixels differ, first at (%lu,%lu): 0x%08x want 0x%08x", (unsigned long)bad,
                          (unsigned long)N, (unsigned long)(first % W), (unsigned long)(first / W), got0, ref[first]);
        }
        DONE();

        STEP(6);   // three queues, eight command buffers in flight on each, completion handlers counted
        {
            enum { NQ = 3, NCB = 8, NW = 1 << 16 };
            __block atomic_int handled = 0;
            NSMutableArray *cbs = [NSMutableArray new], *bufs = [NSMutableArray new];
            for (int qi = 0; qi < NQ; qi++) {
                id<MTLCommandQueue> qq = [dev newCommandQueue];
                for (int c = 0; c < NCB; c++) {
                    const uint32_t seed = (uint32_t)(qi * 1000 + c * 10 + it);
                    id<MTLBuffer> b = [dev newBufferWithLength:NW * 4 options:MTLResourceStorageModeShared];
                    id<MTLCommandBuffer> cb = [qq commandBuffer];
                    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setComputePipelineState:hashPs]; [ce setBuffer:b offset:0 atIndex:0]; [ce setBytes:&seed length:4 atIndex:1];
                    [ce dispatchThreads:MTLSizeMake(NW, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    [ce endEncoding];
                    [cb addCompletedHandler:^(id<MTLCommandBuffer> c_) { atomic_fetch_add(&handled, 1); }];
                    [cb commit];
                    [cbs addObject:cb]; [bufs addObject:b];
                }
            }
            for (NSUInteger k = 0; k < cbs.count; k++) if (*(e = cbDone(cbs[k]))) FAIL("buffer %lu: %s", (unsigned long)k, e);
            for (int spin = 0; spin < 200 && atomic_load(&handled) < NQ * NCB; spin++) usleep(5000);
            if (atomic_load(&handled) != NQ * NCB) FAIL("%d of %d completion handlers ran", atomic_load(&handled), NQ * NCB);
            for (int qi = 0; qi < NQ; qi++) for (int c = 0; c < NCB; c++) {
                const uint32_t seed = (uint32_t)(qi * 1000 + c * 10 + it), *o = [bufs[qi * NCB + c] contents];
                for (NSUInteger i = 0; i < NW; i += 997)
                    if (o[i] != (uint32_t)(i * 0x9E3779B1u + seed)) FAIL("queue %d buffer %d word %lu: 0x%08x", qi, c, (unsigned long)i, o[i]);
            }
        }
        DONE();

        STEP(7);   // churn: 32 x (4 MiB buffer, 1024x1024 texture) made, filled, checked and freed
        {
            for (int k = 0; k < 32; k++) @autoreleasepool {
                id<MTLBuffer> a = [dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared];
                id<MTLBuffer> b = [dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared];
                id<MTLTexture> t = [dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatBGRA8Unorm width:1024 height:1024 mipmapped:NO]];
                if (!a || !b || !t) FAIL("allocation %d failed", k);
                const uint8_t v = (uint8_t)(0x11 * (k % 15 + 1));
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be fillBuffer:a range:NSMakeRange(0, 4 << 20) value:v];
                [be copyFromBuffer:a sourceOffset:0 sourceBytesPerRow:4096 sourceBytesPerImage:4 << 20 sourceSize:MTLSizeMake(1024, 1024, 1)
                         toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1024, 1024, 1)
                           toBuffer:b destinationOffset:0 destinationBytesPerRow:4096 destinationBytesPerImage:4 << 20];
                [be endEncoding];
                [cb commit];
                if (*(e = cbDone(cb))) FAIL("round %d: %s", k, e);
                const uint8_t *o = b.contents;
                for (NSUInteger i = 0; i < (4u << 20); i += 4093) if (o[i] != v) FAIL("round %d byte %lu: 0x%02x want 0x%02x", k, (unsigned long)i, o[i], v);
            }
        }
        DONE();

        printf(" ok %.0f ms\n", nowMs() - ti); fflush(stdout);
        }
    }
out:
    if (fail) printf(" FAIL\nstress: FAIL in %s at iteration %d: %s\n", fail, it, why);
    else {
        printf("stress: %d iterations in %.0f s, all correct; slowest step:", it, (nowMs() - t0) / 1000.0);
        for (int i = 0; i < 8; i++) printf(" %s %.0f ms", names[i], stepMs[i]);
        printf("\n");
    }
#undef STEP
#undef DONE
#undef FAIL
    free(ref);
    if (sa) CFRelease(sa);
    if (sb) CFRelease(sb);
    return rc;
}

// the main display's mode in pixels (a HiDPI mode's backing size), 2560x1600 when there is none
static void displayPixels(NSUInteger *w, NSUInteger *h) {
    *w = 2560; *h = 1600;
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    if (!m) return;
    if (CGDisplayModeGetPixelWidth(m) >= 64 && CGDisplayModeGetPixelHeight(m) >= 64) {
        *w = CGDisplayModeGetPixelWidth(m); *h = CGDisplayModeGetPixelHeight(m);
    }
    CGDisplayModeRelease(m);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("devices: %lu\n", (unsigned long)all.count);
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in all) {
            printf("  %s  class=%s registryID=0x%llx lowPower=%d headless=%d\n", d.name.UTF8String,
                   object_getClassName(d), d.registryID, d.lowPower, d.headless);
            if ([NSStringFromClass([d class]) hasPrefix:@"NVMTL"]) dev = d;
        }
        if (!dev) dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        printf("using %s\n", dev.name.UTF8String);
        if (argc > 1 && !strcmp(argv[1], "list")) return 0;

        if (argc > 1 && !strcmp(argv[1], "render")) return renderTest(dev);
        if (argc > 1 && !strcmp(argv[1], "stress")) {
            NSUInteger w, h;
            displayPixels(&w, &h);
            if (argc > 4) { w = strtoul(argv[3], NULL, 0); h = strtoul(argv[4], NULL, 0); }
            if (w < 64 || h < 64 || w > 8192 || h > 8192) { printf("stress: size %lux%lu out of range\n", (unsigned long)w, (unsigned long)h); return 2; }
            return stressTest(dev, argc > 2 ? atof(argv[2]) : 30.0, w, h);
        }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        if (!lib) { printf("library: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps) { printf("pipeline: %s\n", err.localizedDescription.UTF8String); return 3; }
        const NSUInteger n = 4096;
        id<MTLBuffer> buf = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        memset(buf.contents, 0, n * 4);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        printf("command buffer status %ld error %s\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
        const uint32_t *o = buf.contents;
        NSUInteger ok = 0;
        for (NSUInteger i = 0; i < n; i++) ok += o[i] == (uint32_t)(i * 0x9E3779B1u + 7u);
        printf("results: %lu/%lu correct (out[1]=0x%08x, expected 0x%08x)\n", (unsigned long)ok, (unsigned long)n, o[1],
               (uint32_t)(0x9E3779B1u + 7u));
        return ok == n ? 0 : 4;
    }
}
