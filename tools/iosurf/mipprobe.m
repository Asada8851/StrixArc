// Samples a mipmapped gradient at an explicit level (lod 2: a quarter-resolution image) with linear filtering, as a blurred
// glass backdrop is read, and reports whether the magnified result is smooth or comes out in 4x4 blocks.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; float2 uv; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = float2(q.x, 1 - q.y); return o; }\n"
"fragment float4 fs_lod(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float &lod [[buffer(0)]]) { return t.sample(s, v.uv, level(lod)); }\n"
"fragment float4 fs_pix(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float &lod [[buffer(0)]]) {\n"
"  return t.sample(s, v.uv * float2(t.get_width(), t.get_height())); }\n"
"fragment float4 fs_const(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float &lod [[buffer(0)]]) {\n"
"  constexpr sampler cs(filter::linear, mip_filter::linear, address::clamp_to_edge); return t.sample(cs, v.uv, level(lod)); }\n";

static void run(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *fname, id<MTLSamplerState> smp, id<MTLTexture> tex, float lod)
{
    const int N = 256;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:fname];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { printf("%s: pipeline FAILED %s\n", fname.UTF8String, e.description.UTF8String); return; }
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
    id<MTLTexture> out = [dev newTextureWithDescriptor:od];
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setFragmentTexture:tex atIndex:0]; if (smp) [en setFragmentSamplerState:smp atIndex:0];
    [en setFragmentBytes:&lod length:4 atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    static uint8_t px[256 * 256 * 4];
    [out getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
    // along a row of the horizontal gradient: a smooth result changes almost every pixel, a 4x4-block one every 4th
    int steps = 0, flat = 0; const int y = N / 2;
    for (int x = 1; x < N; x++) { int d = px[(y * N + x) * 4] - px[(y * N + x - 1) * 4]; if (d) steps++; else flat++; }
    printf("%-9s lod %.1f: red along a row %3d %3d %3d %3d %3d %3d %3d %3d ... changes %d of %d pixels -> %s\n", fname.UTF8String, lod,
           px[(y*N+64)*4], px[(y*N+65)*4], px[(y*N+66)*4], px[(y*N+67)*4], px[(y*N+68)*4], px[(y*N+69)*4], px[(y*N+70)*4], px[(y*N+71)*4],
           steps, N - 1, steps > N * 2 / 3 ? "SMOOTH" : steps < N / 3 ? "BLOCKY" : "mixed");
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device: %s\n", dev.name.UTF8String);
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("library FAILED %s\n", e.description.UTF8String); return 1; }
        const int N = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        static uint8_t g[256 * 256 * 4];
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { uint8_t *p = g + (y * N + x) * 4; p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 0; p[3] = 255; }
        [tex replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:g bytesPerRow:N * 4];
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl generateMipmapsForTexture:tex]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterLinear;
        id<MTLSamplerState> lin = [dev newSamplerStateWithDescriptor:sd];
        run(dev, lib, @"fs_lod", lin, tex, 0.0f);
        run(dev, lib, @"fs_lod", lin, tex, 2.0f);
        run(dev, lib, @"fs_const", nil, tex, 2.0f);
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        run(dev, lib, @"fs_lod", [dev newSamplerStateWithDescriptor:sd], tex, 2.0f);
        // pixel-coordinate samplers magnifying a 64x64 image: CoreAnimation's own kind
        MTLTextureDescriptor *sdsc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
        id<MTLTexture> small = [dev newTextureWithDescriptor:sdsc];
        static uint8_t sg[64 * 64 * 4];
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = sg + (y * 64 + x) * 4; p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 4); p[2] = 0; p[3] = 255; }
        [small replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:sg bytesPerRow:64 * 4];
        MTLSamplerDescriptor *pd = [MTLSamplerDescriptor new];
        pd.normalizedCoordinates = NO; pd.minFilter = pd.magFilter = MTLSamplerMinMagFilterLinear;
        run(dev, lib, @"fs_pix", [dev newSamplerStateWithDescriptor:pd], small, 0.0f);
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        run(dev, lib, @"fs_lod", [dev newSamplerStateWithDescriptor:sd], small, 0.0f);
    }
    return 0;
}
