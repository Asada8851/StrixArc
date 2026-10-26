// Function-constant-gated textures at indices 3 and 4 (CoreAnimation's fixed_frag_lpf_cpf shape): solid colours bound
// at 0 red, 1 green, 3 blue, 4 white; the shader returns t3 + t4 * 0.25, so the right textures give (64,64,255).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"constant bool use_a [[function_constant(0)]];\nconstant bool use_b [[function_constant(1)]];\nconstant bool use_c [[function_constant(2)]];\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
"fragment float4 fs(V v [[stage_in]], texture2d<float> t0 [[texture(0), function_constant(use_a)]], texture2d<float> t1 [[texture(1), function_constant(use_a)]],\n"
"                   texture2d<float> t3 [[texture(3), function_constant(use_b)]], texture2d<float> t4 [[texture(4), function_constant(use_c)]], sampler s [[sampler(0)]]) {\n"
"  float4 r = float4(0, 0, 0, 1); float2 uv = float2(0.5);\n"
"  if (use_a) r += t0.sample(s, uv) + t1.sample(s, uv);\n"
"  if (use_b) r.rgb += t3.sample(s, uv).rgb;\n"
"  if (use_c) r.rgb += t4.sample(s, uv).rgb * 0.25;\n"
"  return r; }\n";
static id<MTLTexture> solid(id<MTLDevice> dev, uint8_t r, uint8_t g, uint8_t b)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:d]; uint8_t px[64];
    for (int i = 0; i < 16; i++) { px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255; }
    [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16]; return t;
}
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("library FAILED %s\n", e.description.UTF8String); return 1; }
        id<MTLTexture> T[5] = { solid(dev, 255, 0, 0), solid(dev, 0, 255, 0), nil, solid(dev, 0, 0, 255), solid(dev, 255, 255, 255) };
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        bool combos[3][3] = { { false, true, true }, { true, true, false }, { false, true, false } };
        for (int c = 0; c < 3; c++) {
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            [cv setConstantValue:&combos[c][0] type:MTLDataTypeBool atIndex:0];
            [cv setConstantValue:&combos[c][1] type:MTLDataTypeBool atIndex:1];
            [cv setConstantValue:&combos[c][2] type:MTLDataTypeBool atIndex:2];
            id<MTLFunction> f = [lib newFunctionWithName:@"fs" constantValues:cv error:&e];
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = f; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!ps) { printf("pipeline FAILED %s\n", e.description.UTF8String); continue; }
            MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
            od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
            id<MTLTexture> out = [dev newTextureWithDescriptor:od];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:ps];
            for (int i = 0; i < 5; i++) if (T[i]) [en setFragmentTexture:T[i] atIndex:i];
            [en setFragmentSamplerState:smp atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            uint8_t px[4]; [out getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
            int er = (combos[c][0] ? 255 : 0) + (combos[c][2] ? 64 : 0), eg = (combos[c][0] ? 255 : 0) + (combos[c][2] ? 64 : 0), eb = (combos[c][1] ? 255 : 0) + (combos[c][2] ? 64 : 0);
            er = er > 255 ? 255 : er; eg = eg > 255 ? 255 : eg; eb = eb > 255 ? 255 : eb;
            printf("use_a %d use_b %d use_c %d: got (%3d,%3d,%3d) want (%3d,%3d,%3d) %s\n", combos[c][0], combos[c][1], combos[c][2],
                   px[0], px[1], px[2], er, eg, eb, (abs(px[0] - er) < 4 && abs(px[1] - eg) < 4 && abs(px[2] - eb) < 4) ? "OK" : "WRONG TEXTURES");
        }
    }
    return 0;
}
