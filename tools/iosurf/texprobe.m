// Two things RenderBox (SwiftUI, Tahoe's layered icons) leans on, checked against the expected pixels:
//  - textures over an MTLBuffer (newTextureWithDescriptor:offset:bytesPerRow:): sampled after a CPU fill, and drawn into
//    as a render target then read back through the buffer, for several formats, sizes and row pitches;
//  - framebuffer fetch: a fragment shader reading [[color(0)]] of a cleared attachment.
// Usage: texprobe
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <math.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; float2 uv; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = float2(q.x, 1 - q.y); return o; }\n"
"fragment float4 fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::nearest); return t.sample(s, v.uv); }\n"
"fragment float4 solid(V v [[stage_in]]) { return float4(0.25, 0.5, 0.75, 1.0); }\n"
"fragment float4 fetch(V v [[stage_in]], float4 c [[color(0)]]) { return float4(1.0 - c.r, c.g * 0.5, c.b, 1.0); }\n";

static int fails;

// The value the CPU writes at texel (x, y), channel k, as a float in 0..1.
static float pat(int x, int y, int k, int w, int h) { return k == 0 ? (x + 0.5f) / w : k == 1 ? (y + 0.5f) / h : k == 2 ? 0.5f : 1.0f; }

static uint16_t f2h(float f) { __fp16 h = (__fp16)f; uint16_t u; memcpy(&u, &h, 2); return u; }

typedef struct { MTLPixelFormat fmt; const char *name; int ch, bytes; int half, bgra; } Fmt;

static void check_sample(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> ps, Fmt f, int w, int h, int pitch)
{
    const int bpp = f.ch * f.bytes;
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)pitch * h options:MTLResourceStorageModeShared];
    uint8_t *p = buf.contents;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int k = 0; k < f.ch; k++) {
        const int kk = f.bgra ? (k == 0 ? 2 : k == 2 ? 0 : k) : k;
        const float v = pat(x, y, k, w, h);
        uint8_t *d = p + (size_t)y * pitch + x * bpp + kk * f.bytes;
        if (f.half) { uint16_t u = f2h(v); memcpy(d, &u, 2); }
        else if (f.bytes == 4) memcpy(d, &v, 4);
        else *d = (uint8_t)lrintf(v * 255);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:pitch];
    if (!t) { printf("  sample %-8s %3dx%-3d pitch %4d: NO TEXTURE\n", f.name, w, h, pitch); fails++; return; }
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:w height:h mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
    id<MTLTexture> out = [dev newTextureWithDescriptor:od];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setFragmentTexture:t atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    float *px = malloc((size_t)w * h * 16);
    [out getBytes:px bytesPerRow:w * 16 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    int bad = 0; float worst = 0;
    const int xs[] = { 0, w / 3, w - 1 }, ys[] = { 0, h / 2, h - 1 };
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int k = 0; k < f.ch; k++) {
        const float want = pat(xs[a], ys[b], k, w, h), got = px[((size_t)ys[b] * w + xs[a]) * 4 + k];
        const float err = fabsf(want - got); if (err > worst) worst = err;
        if (err > 0.01f) bad++;
    }
    free(px);
    printf("  sample %-8s %3dx%-3d pitch %4d: %s (worst error %.3f)\n", f.name, w, h, pitch, bad ? "WRONG" : "ok", worst);
    if (bad) fails++;
}

static void check_render(id<MTLDevice> dev, id<MTLCommandQueue> q, Fmt f, int w, int h, int pitch, id<MTLLibrary> lib)
{
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"solid"];
    pd.colorAttachments[0].pixelFormat = f.fmt;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)pitch * h options:MTLResourceStorageModeShared];
    memset(buf.contents, 0, buf.length);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:pitch];
    if (!t || !ps) { printf("  render %-8s %3dx%-3d pitch %4d: NO %s\n", f.name, w, h, pitch, t ? "PIPELINE" : "TEXTURE"); fails++; return; }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    const float want[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    int bad = 0; float worst = 0;
    const int xs[] = { 0, w / 2, w - 1 }, ys[] = { 0, h / 2, h - 1 };
    const uint8_t *p = buf.contents;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int k = 0; k < f.ch; k++) {
        const int kk = f.bgra ? (k == 0 ? 2 : k == 2 ? 0 : k) : k;
        const uint8_t *s = p + (size_t)ys[b] * pitch + xs[a] * f.ch * f.bytes + kk * f.bytes;
        float got;
        if (f.half) { uint16_t u; memcpy(&u, s, 2); __fp16 hh; memcpy(&hh, &u, 2); got = (float)hh; }
        else if (f.bytes == 4) memcpy(&got, s, 4);
        else got = *s / 255.0f;
        const float err = fabsf(want[k] - got); if (err > worst) worst = err;
        if (err > 0.01f) bad++;
    }
    printf("  render %-8s %3dx%-3d pitch %4d: %s (worst error %.3f)\n", f.name, w, h, pitch, bad ? "WRONG" : "ok", worst);
    if (bad) fails++;
}

static void check_fetch(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fetch"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { printf("  framebuffer fetch: NO PIPELINE (%s)\n", e.localizedDescription.UTF8String); fails++; return; }
    const int N = 64;
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
    id<MTLTexture> out = [dev newTextureWithDescriptor:od];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    uint8_t px[N * N * 4];
    [out getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
    const uint8_t *c = px + (N / 2 * N + N / 2) * 4;
    const int ok = abs(c[0] - 191) < 4 && abs(c[1] - 64) < 4 && abs(c[2] - 191) < 4;
    printf("  framebuffer fetch: %3d %3d %3d %3d (want 191 64 191 255) %s\n", c[0], c[1], c[2], c[3], ok ? "ok" : "WRONG");
    if (!ok) fails++;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const Fmt fmts[] = {
            { MTLPixelFormatRGBA8Unorm, "RGBA8", 4, 1, 0, 0 }, { MTLPixelFormatBGRA8Unorm, "BGRA8", 4, 1, 0, 1 },
            { MTLPixelFormatRGBA16Float, "RGBA16F", 4, 2, 1, 0 }, { MTLPixelFormatR8Unorm, "R8", 1, 1, 0, 0 },
            { MTLPixelFormatRG8Unorm, "RG8", 2, 1, 0, 0 }, { MTLPixelFormatRGBA32Float, "RGBA32F", 4, 4, 0, 0 },
        };
        const int sizes[][2] = { { 128, 128 }, { 32, 32 }, { 100, 37 } };
        printf("buffer textures:\n");
        for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
            for (unsigned s = 0; s < 3; s++) {
                const int w = sizes[s][0], h = sizes[s][1], bpp = fmts[i].ch * fmts[i].bytes;
                const int tight = w * bpp, al = (tight + 255) / 256 * 256;
                check_sample(dev, q, ps, fmts[i], w, h, al);
                if (tight != al && tight % 16 == 0) {   // below Metal's minimumLinearTextureAlignment (256): reported, not counted
                    const int before = fails; check_sample(dev, q, ps, fmts[i], w, h, tight); fails = before;
                }
                check_render(dev, q, fmts[i], w, h, al, lib);
            }
        printf("framebuffer fetch:\n");
        check_fetch(dev, q, lib);
        printf("%d failure(s)\n", fails);
    }
    return fails ? 1 : 0;
}
