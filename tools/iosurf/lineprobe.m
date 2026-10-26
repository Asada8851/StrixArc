// Draws a line list, a line strip and points into a 64x64 target and checks the pixels: the plugin used to drop
// every point/line draw on RADV.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; float sz [[point_size]]; };\n"
"vertex V vs(uint i [[vertex_id]], constant float2 *pts [[buffer(0)]]) { V o; o.p = float4(pts[i], 0, 1); o.sz = 4; return o; }\n"
"fragment float4 fs() { return float4(1, 1, 1, 1); }\n";

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("library FAILED %s\n", e.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { printf("pipeline FAILED %s\n", e.description.UTF8String); return 1; }
        const int N = 64;
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged;
        struct { MTLPrimitiveType t; const char *name; float pts[8]; int n; } cases[] = {
            { MTLPrimitiveTypeLine, "line list (horizontal)", { -0.9f, 0.0f, 0.9f, 0.0f }, 2 },
            { MTLPrimitiveTypeLineStrip, "line strip (vertical)", { 0.0f, -0.9f, 0.0f, 0.0f, 0.0f, 0.9f }, 3 },
            { MTLPrimitiveTypePoint, "points (4 px)", { -0.5f, -0.5f, 0.5f, 0.5f }, 2 },
            { MTLPrimitiveTypeTriangle, "triangle (control)", { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f }, 3 },
        };
        id<MTLCommandQueue> q = [dev newCommandQueue];
        for (unsigned c = 0; c < 4; c++) {
            id<MTLTexture> out = [dev newTextureWithDescriptor:od];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:ps]; [en setVertexBytes:cases[c].pts length:sizeof cases[c].pts atIndex:0];
            [en drawPrimitives:cases[c].t vertexStart:0 vertexCount:cases[c].n]; [en endEncoding];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            static uint8_t px[64 * 64 * 4];
            [out getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
            int lit = 0; for (int i = 0; i < N * N; i++) lit += px[i * 4] > 128;
            printf("%-24s: %4d lit pixels -> %s (cb status %ld)\n", cases[c].name, lit, lit ? "DRAWN" : "NOT DRAWN", (long)cb.status);
        }
    }
    return 0;
}
