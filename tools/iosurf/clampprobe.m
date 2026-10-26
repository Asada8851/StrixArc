// setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex: on a mipmapped 1-pixel stripe texture: with lodMinClamp 2 the
// GPU must read level 2 (an even grey), without it level 0 (the stripes).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; float2 uv; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = float2(q.x, 1 - q.y); return o; }\n"
"fragment float4 fs(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, v.uv); }\n";
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        const int N = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        static uint8_t g[256 * 256 * 4];
        const int grad = getenv("GRAD") != NULL;   // GRAD=1: a horizontal gradient, so level 2 is not uniform
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { uint8_t *p = g + (y * N + x) * 4; p[0] = p[1] = p[2] = grad ? (uint8_t)x : (x & 1) ? 255 : 0; p[3] = 255; }
        [tex replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:g bytesPerRow:N * 4];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl generateMipmapsForTexture:tex]; [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterNearest; sd.mipFilter = MTLSamplerMipFilterLinear;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        for (int clamp = 0; clamp < 2; clamp++) {
            MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:NO];
            od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
            id<MTLTexture> out = [dev newTextureWithDescriptor:od];
            cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:ps]; [en setFragmentTexture:tex atIndex:0];
            if (clamp) [en setFragmentSamplerState:smp lodMinClamp:2 lodMaxClamp:2 atIndex:0]; else [en setFragmentSamplerState:smp atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
            bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
            static uint8_t px[256 * 256 * 4];
            [out getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
            const int y = N / 2;
            if (grad) { printf("%s gradient: %3d %3d %3d %3d %3d %3d %3d %3d -> %s\n", clamp ? "lodMinClamp 2" : "no clamp     ",
                   px[(y*N+100)*4], px[(y*N+101)*4], px[(y*N+102)*4], px[(y*N+103)*4], px[(y*N+104)*4], px[(y*N+105)*4], px[(y*N+106)*4], px[(y*N+107)*4],
                   px[(y*N+100)*4] == px[(y*N+101)*4] && px[(y*N+101)*4] == px[(y*N+102)*4] ? "BLOCKS (magnification filter)" : "smooth (minification filter)"); continue; }
            printf("%s: %3d %3d %3d %3d %3d %3d -> %s\n", clamp ? "lodMinClamp 2" : "no clamp     ",
                   px[(y*N+100)*4], px[(y*N+101)*4], px[(y*N+102)*4], px[(y*N+103)*4], px[(y*N+104)*4], px[(y*N+105)*4],
                   abs(px[(y*N+100)*4] - px[(y*N+101)*4]) > 100 ? "stripes (level 0)" : "grey (reduced level)");
        }
    }
    return 0;
}
