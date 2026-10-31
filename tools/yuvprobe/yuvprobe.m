// yuvprobe: do YUV video frames survive the Metal plugin? Video players hand the GPU biplanar IOSurfaces (420v/420f,
// 10-bit x420), one MTLTexture per plane, and nothing else in tools/ ever wraps a plane other than 0. Every check below
// writes a known pattern on the CPU, lets the GPU read or draw it, reads the result back and compares it on the CPU.
// Build: clang -fobjc-arc -framework Foundation -framework Metal -framework CoreVideo -framework IOSurface
//   -framework CoreMedia -framework AVFoundation -framework VideoToolbox -framework QuartzCore -framework AppKit
//   -framework CoreAudio
//   -o yuvprobe yuvprobe.m
//   yuvprobe [all]            synthetic surfaces: 420v/420f/x420 at 3 sizes, wrapped directly and through
//                             CVMetalTextureCache; plane readback, YUV->RGB with read/nearest/linear; GPU writes into
//                             planes; a CVPixelBufferPool cycled like a decoder's (stale frames); packed 4:2:2
//   yuvprobe quick            the same for 420v 1920x1080 only
//   yuvprobe vt               VideoToolbox: is there a hardware H.264/HEVC decoder
//   yuvprobe asset <file> [frames]   frames decoded by VideoToolbox (AVAssetReader, as QuickTime decodes them): every
//                             plane and the YUV->RGB of each frame checked against the CPU; VT's own BGRA output compared
//   yuvprobe play <file> [s] [noaudio]  an AVPlayerLayer window (QuickTime's path through CoreAnimation and
//                             WindowServer); reports the player's status, why it waits, and whether its clock ran.
//                             noaudio: the video track alone (AVPlayer then runs on the host clock, not the audio device's)
//   yuvprobe audio            CoreAudio's devices: the default output, whether it is alive and running, its streams
// Exit status: 0 all passed, 1 a check failed, 3 the GPU did not complete a command buffer in 5 s, 4 a call hung.
// Lines start with PASS / FAIL / GAP (a format the plugin does not carry) / INFO.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <CoreMedia/CoreMedia.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <QuartzCore/QuartzCore.h>
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQ;
static id<MTLComputePipelineState> gRd8, gRd16, gRd32;
static id<MTLRenderPipelineState> gCvtRead, gCvtNear, gCvtLin, gWy[4], gWc[4];   // [0] R8/RG8, [1] R16/RG16
static id<MTLSamplerState> gNear, gLin;
static CVMetalTextureCacheRef gCache;
static int gPass, gFail, gGap;

// ---- watchdog: a call into the plugin that never returns leaves the step it hung in on the last line
static const char *volatile gStep = "start";
static volatile double gStepAt;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void step(const char *s) { gStep = s; gStepAt = now_s(); }
static void *watchdog(void *arg) {
    (void)arg;
    for (;;) { sleep(2); if (gStepAt && now_s() - gStepAt > 30) { printf("HANG: no progress for 30 s in '%s'\n", gStep); fflush(stdout); _exit(4); } }
    return NULL;
}

static void result(const char *kind, NSString *msg) {
    if (!strcmp(kind, "PASS")) gPass++; else if (!strcmp(kind, "FAIL")) gFail++; else if (!strcmp(kind, "GAP")) gGap++;
    printf("%-4s %s\n", kind, msg.UTF8String); fflush(stdout);
}

static BOOL run(id<MTLCommandBuffer> cb, NSString **why) {
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; dispatch_semaphore_signal(s); }];
    [cb commit];
    if (dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
        printf("HANG: the GPU did not complete '%s' in 5 s\n", gStep); fflush(stdout); _exit(3);
    }
    if (cb.status != MTLCommandBufferStatusCompleted) {
        if (why) *why = [NSString stringWithFormat:@"command buffer status %ld: %@", (long)cb.status, cb.error.localizedDescription];
        return NO;
    }
    return YES;
}

static NSString *const kSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { float scale, yoff, ymul, coff, rcr, gcb, gcr, bcb; uint seed, tenbit, pad0, pad1; };
kernel void rd8(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]],
                constant uint2 &sz [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= sz.x || g.y >= sz.y) return;
    float4 v = t.read(g);
    o[g.y * sz.x + g.x] = uint(v.r * 255.0 + 0.5) | (uint(v.g * 255.0 + 0.5) << 8);
}
kernel void rd16(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]],
                 constant uint2 &sz [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= sz.x || g.y >= sz.y) return;
    float4 v = t.read(g);
    o[g.y * sz.x + g.x] = uint(v.r * 65535.0 + 0.5) | (uint(v.g * 65535.0 + 0.5) << 16);
}
kernel void rd32(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]],
                 constant uint2 &sz [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= sz.x || g.y >= sz.y) return;
    float4 v = t.read(g);
    o[g.y * sz.x + g.x] = uint(v.r * 255.0 + 0.5) | (uint(v.g * 255.0 + 0.5) << 8) | (uint(v.b * 255.0 + 0.5) << 16) | (uint(v.a * 255.0 + 0.5) << 24);
}
struct VO { float4 pos [[position]]; };
vertex VO vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2); VO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;
}
static float3 cvt(float y, float2 c, constant P &p) {
    float Y = (y * p.scale - p.yoff) * p.ymul, cb = c.x * p.scale - p.coff, cr = c.y * p.scale - p.coff;
    return clamp(float3(Y + p.rcr * cr, Y + p.gcb * cb + p.gcr * cr, Y + p.bcb * cb) / 255.0, 0.0, 1.0);
}
fragment float4 fs_read(VO in [[stage_in]], texture2d<float, access::read> ty [[texture(0)]],
                        texture2d<float, access::read> tc [[texture(1)]], constant P &p [[buffer(0)]]) {
    uint2 q = uint2(in.pos.xy);
    return float4(cvt(ty.read(q).r, tc.read(q / 2).rg, p), 1.0);
}
fragment float4 fs_near(VO in [[stage_in]], texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
                        sampler s [[sampler(0)]], constant P &p [[buffer(0)]]) {
    float2 ys = float2(ty.get_width(), ty.get_height()), cs = float2(tc.get_width(), tc.get_height());
    float2 q = floor(in.pos.xy);
    return float4(cvt(ty.sample(s, (q + 0.5) / ys).r, tc.sample(s, (floor(q / 2.0) + 0.5) / cs).rg, p), 1.0);
}
fragment float4 fs_lin(VO in [[stage_in]], texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
                       sampler s [[sampler(0)]], constant P &p [[buffer(0)]]) {
    float2 ys = float2(ty.get_width(), ty.get_height());
    float2 uv = in.pos.xy / ys;   // the luma pixel's centre: chroma filtered between its texels, as a player samples it
    return float4(cvt(ty.sample(s, uv).r, tc.sample(s, uv).rg, p), 1.0);
}
// the GPU as the writer of the planes (a GPU decoder or VideoToolbox's converter): the same pattern the CPU writes
static uint py(uint x, uint y, uint s, uint t) { return t ? 64 + (x * 3 + y * 5 + s * 7) % 877 : 16 + (x * 3 + y * 5 + s * 7) % 220; }
static uint pb(uint x, uint y, uint s, uint t) { return t ? 64 + (x * 7 + y * 2 + s * 13) % 897 : 16 + (x * 7 + y * 2 + s * 13) % 225; }
static uint pr(uint x, uint y, uint s, uint t) { return t ? 64 + (x * 2 + y * 9 + s * 29) % 897 : 16 + (x * 2 + y * 9 + s * 29) % 225; }
fragment float4 fs_wy(VO in [[stage_in]], constant P &p [[buffer(0)]]) {
    uint2 q = uint2(in.pos.xy); uint v = py(q.x, q.y, p.seed, p.tenbit);
    return float4(p.tenbit ? float(v << 6) / 65535.0 : float(v) / 255.0, 0.0, 0.0, 1.0);
}
fragment float4 fs_wc(VO in [[stage_in]], constant P &p [[buffer(0)]]) {
    uint2 q = uint2(in.pos.xy); uint b = pb(q.x, q.y, p.seed, p.tenbit), r = pr(q.x, q.y, p.seed, p.tenbit);
    return p.tenbit ? float4(float(b << 6) / 65535.0, float(r << 6) / 65535.0, 0.0, 1.0) : float4(float(b) / 255.0, float(r) / 255.0, 0.0, 1.0);
}
)MSL";

// ---- the pattern (the shader's py/pb/pr), 8-bit codes or 10-bit codes
static unsigned PY(unsigned x, unsigned y, unsigned s, int t) { return t ? 64 + (x * 3 + y * 5 + s * 7) % 877 : 16 + (x * 3 + y * 5 + s * 7) % 220; }
static unsigned PB(unsigned x, unsigned y, unsigned s, int t) { return t ? 64 + (x * 7 + y * 2 + s * 13) % 897 : 16 + (x * 7 + y * 2 + s * 13) % 225; }
static unsigned PR(unsigned x, unsigned y, unsigned s, int t) { return t ? 64 + (x * 2 + y * 9 + s * 29) % 897 : 16 + (x * 2 + y * 9 + s * 29) % 225; }

typedef struct { float scale, yoff, ymul, coff, rcr, gcb, gcr, bcb; uint32_t seed, tenbit, pad0, pad1; } P;
static P params(OSType fmt, int tenbit, CFTypeRef matrix) {
    const int full = fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || fmt == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    const int bt601 = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4);
    // Kr/Kb of BT.709 or BT.601; video range scales luma by 255/219 and chroma by 255/224
    const double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1 - kr - kb;
    const double cs = full ? 1.0 : 255.0 / 224.0;
    P p = { tenbit ? 65535.0f / 256.0f : 255.0f, full ? 0.0f : 16.0f, full ? 1.0f : (float)(255.0 / 219.0), 128.0f,
            (float)(2 * (1 - kr) * cs), (float)(-2 * kb * (1 - kb) / kg * cs), (float)(-2 * kr * (1 - kr) / kg * cs),
            (float)(2 * (1 - kb) * cs), 0, (uint32_t)tenbit, 0, 0 };
    return p;
}
// what the shader's cvt() gives, in 8-bit codes, from 8-bit-equivalent inputs
static void cvt_cpu(const P *p, double y, double cb, double cr, double rgb[3]) {
    const double Y = (y - p->yoff) * p->ymul; cb -= p->coff; cr -= p->coff;
    rgb[0] = Y + p->rcr * cr; rgb[1] = Y + p->gcb * cb + p->gcr * cr; rgb[2] = Y + p->bcb * cb;
    for (int i = 0; i < 3; i++) rgb[i] = rgb[i] < 0 ? 0 : rgb[i] > 255 ? 255 : rgb[i];
}

static int is10(OSType f) { return f == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; }
static NSString *fourcc(OSType f) { char c[5] = { (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f, 0 }; return @(c); }

// the CPU's view of a locked biplanar buffer: 8-bit-equivalent Y/Cb/Cr at (x, y)
typedef struct { const uint8_t *b0, *b1; size_t r0, r1; int ten; } Planes;
static double ly(const Planes *pl, size_t x, size_t y) {
    return pl->ten ? (((const uint16_t *)(pl->b0 + y * pl->r0))[x] >> 6) / 4.0 : pl->b0[y * pl->r0 + x];
}
static double lc(const Planes *pl, size_t x, size_t y, int k) {   // k 0 = Cb, 1 = Cr; x, y in chroma texels
    return pl->ten ? (((const uint16_t *)(pl->b1 + y * pl->r1))[2 * x + k] >> 6) / 4.0 : pl->b1[y * pl->r1 + 2 * x + k];
}
// For the readback: exact codes as the GPU should see them (16-bit containers keep the 10 bits in their top bits)
static uint32_t raw_y(const Planes *pl, size_t x, size_t y) { return pl->ten ? ((const uint16_t *)(pl->b0 + y * pl->r0))[x] : pl->b0[y * pl->r0 + x]; }
static uint32_t raw_c(const Planes *pl, size_t x, size_t y) {
    if (pl->ten) { const uint16_t *r = (const uint16_t *)(pl->b1 + y * pl->r1); return r[2 * x] | ((uint32_t)r[2 * x + 1] << 16); }
    return pl->b1[y * pl->r1 + 2 * x] | ((uint32_t)pl->b1[y * pl->r1 + 2 * x + 1] << 8);
}

static void fill(CVPixelBufferRef pb, unsigned seed) {
    CVPixelBufferLockBaseAddress(pb, 0);
    const int t = is10(CVPixelBufferGetPixelFormatType(pb));
    const size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
    const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
    uint8_t *b0 = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *b1 = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t r0 = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), r1 = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
        if (t) ((uint16_t *)(b0 + y * r0))[x] = (uint16_t)(PY((unsigned)x, (unsigned)y, seed, 1) << 6);
        else b0[y * r0 + x] = (uint8_t)PY((unsigned)x, (unsigned)y, seed, 0);
    }
    for (size_t y = 0; y < ch; y++) for (size_t x = 0; x < cw; x++) {
        if (t) { uint16_t *r = (uint16_t *)(b1 + y * r1); r[2 * x] = (uint16_t)(PB((unsigned)x, (unsigned)y, seed, 1) << 6); r[2 * x + 1] = (uint16_t)(PR((unsigned)x, (unsigned)y, seed, 1) << 6); }
        else { b1[y * r1 + 2 * x] = (uint8_t)PB((unsigned)x, (unsigned)y, seed, 0); b1[y * r1 + 2 * x + 1] = (uint8_t)PR((unsigned)x, (unsigned)y, seed, 0); }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static CVPixelBufferRef newbuf(OSType fmt, size_t w, size_t h) {
    NSDictionary *a = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferCreate(NULL, w, h, fmt, (__bridge CFDictionaryRef)a, &pb);
    return r == kCVReturnSuccess ? pb : NULL;
}
static NSString *geometry(CVPixelBufferRef pb) {
    IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
    NSMutableString *g = [NSMutableString stringWithFormat:@"surface %u %s %zux%zu alloc %zu", s ? IOSurfaceGetID(s) : 0,
        fourcc(CVPixelBufferGetPixelFormatType(pb)).UTF8String, CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), s ? IOSurfaceGetAllocSize(s) : 0];
    const size_t n = CVPixelBufferGetPlaneCount(pb);
    for (size_t i = 0; i < n && s; i++)
        [g appendFormat:@" | p%zu %zux%zu row %zu at +%zu", i, IOSurfaceGetWidthOfPlane(s, i), IOSurfaceGetHeightOfPlane(s, i), IOSurfaceGetBytesPerRowOfPlane(s, i),
            (size_t)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, i) - (uint8_t *)IOSurfaceGetBaseAddress(s))];
    return g;
}


// one plane as an MTLTexture: directly on the IOSurface, or through CVMetalTextureCache (what AVFoundation clients do)
typedef struct { id<MTLTexture> tex; CVMetalTextureRef cv; } Wrap;
static void unwrap(Wrap *w) { w->tex = nil; if (w->cv) CFRelease(w->cv); w->cv = NULL; }
static Wrap wrap(CVPixelBufferRef pb, size_t plane, MTLPixelFormat f, size_t w, size_t h, int viaCache, MTLTextureUsage usage) {
    Wrap r = { nil, NULL };
    if (viaCache) {
        NSDictionary *a = @{ (id)kCVMetalTextureUsage: @(usage) };
        if (CVMetalTextureCacheCreateTextureFromImage(NULL, gCache, pb, (__bridge CFDictionaryRef)a, f, w, h, plane, &r.cv) == kCVReturnSuccess && r.cv)
            r.tex = CVMetalTextureGetTexture(r.cv);
    } else {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
        d.usage = usage;
        r.tex = [gDev newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(pb) plane:plane];
    }
    return r;
}

// the texture's texels as the GPU reads them, one uint32 each (rd8: r | g << 8, rd16: r | g << 16, rd32: rgba)
static id<MTLBuffer> gpu_read(id<MTLTexture> t, id<MTLComputePipelineState> ps, size_t w, size_t h, NSString **why) {
    id<MTLBuffer> o = [gDev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    memset(o.contents, 0xA5, w * h * 4);
    const uint32_t sz[2] = { (uint32_t)w, (uint32_t)h };
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps]; [e setTexture:t atIndex:0]; [e setBuffer:o offset:0 atIndex:0]; [e setBytes:sz length:sizeof sz atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    return run(cb, why) ? o : nil;
}

// YUV -> RGB into a BGRA target, read back through a blit
static id<MTLBuffer> gpu_convert(id<MTLRenderPipelineState> ps, id<MTLTexture> ty, id<MTLTexture> tc, id<MTLSamplerState> s,
                                 const P *p, size_t w, size_t h, NSString **why) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [gDev newTextureWithDescriptor:d];
    id<MTLBuffer> o = [gDev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e setFragmentTexture:ty atIndex:0]; [e setFragmentTexture:tc atIndex:1];
    if (s) [e setFragmentSamplerState:s atIndex:0];
    [e setFragmentBytes:p length:sizeof *p atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:o destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [b endEncoding];
    return run(cb, why) ? o : nil;
}

static Planes lock_planes(CVPixelBufferRef pb) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    Planes pl = { CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1),
                  CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                  is10(CVPixelBufferGetPixelFormatType(pb)) };
    return pl;
}
static void unlock_planes(CVPixelBufferRef pb) { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); }

// GPU readback of one plane against the locked buffer: number of wrong texels, the first ones in *first
static size_t cmp_plane(const uint32_t *got, const Planes *pl, int chroma, size_t w, size_t h, NSMutableString *first) {
    size_t bad = 0;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
        const uint32_t want = chroma ? raw_c(pl, x, y) : raw_y(pl, x, y), g = got[y * w + x];
        if (g != want && bad++ < 3) [first appendFormat:@" (%zu,%zu) want 0x%x got 0x%x;", x, y, want, g];
    }
    return bad;
}

// a converted frame against the CPU's conversion; linear: chroma filtered bilinearly at each luma pixel's centre
static size_t cmp_rgb(const uint8_t *bgra, const Planes *pl, const P *p, size_t w, size_t h, size_t cw, size_t ch,
                      int linear, int tol, int *maxd, NSMutableString *first) {
    size_t bad = 0; *maxd = 0;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
        double cb, cr;
        if (!linear) { cb = lc(pl, x / 2, y / 2, 0); cr = lc(pl, x / 2, y / 2, 1); }
        else {
            const double u = (x + 0.5) * cw / w - 0.5, v = (y + 0.5) * ch / h - 0.5;
            const double fu = floor(u), fv = floor(v), au = u - fu, av = v - fv;
            long x0 = (long)fu, y0 = (long)fv, x1 = x0 + 1, y1 = y0 + 1;
            #define CL(a, n) ((a) < 0 ? 0 : (a) >= (long)(n) ? (long)(n) - 1 : (a))
            x0 = CL(x0, cw); x1 = CL(x1, cw); y0 = CL(y0, ch); y1 = CL(y1, ch);
            #undef CL
            double c[2];
            for (int k = 0; k < 2; k++)
                c[k] = (1 - av) * ((1 - au) * lc(pl, x0, y0, k) + au * lc(pl, x1, y0, k)) + av * ((1 - au) * lc(pl, x0, y1, k) + au * lc(pl, x1, y1, k));
            cb = c[0]; cr = c[1];
        }
        double rgb[3]; cvt_cpu(p, ly(pl, x, y), cb, cr, rgb);
        const uint8_t *px = bgra + (y * w + x) * 4;
        const int got[3] = { px[2], px[1], px[0] };
        int d = 0;
        for (int i = 0; i < 3; i++) { int e = abs(got[i] - (int)lrint(rgb[i])); if (e > d) d = e; }
        if (d > *maxd) *maxd = d;
        if (d > tol && bad++ < 3) [first appendFormat:@" (%zu,%zu) want %ld,%ld,%ld got %d,%d,%d;", x, y,
                                    lrint(rgb[0]), lrint(rgb[1]), lrint(rgb[2]), got[0], got[1], got[2]];
    }
    return bad;
}

static void cmp_report(NSString *tag, const char *what, size_t bad, size_t total, NSString *first, NSString *extra) {
    if (!bad) result("PASS", [NSString stringWithFormat:@"%@ %s%@", tag, what, extra ? extra : @""]);
    else result("FAIL", [NSString stringWithFormat:@"%@ %s: %zu of %zu wrong%@%@", tag, what, bad, total, first, extra ? extra : @""]);
}

static MTLPixelFormat yfmt(int t) { return t ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm; }
static MTLPixelFormat cfmt(int t) { return t ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm; }

// both planes read back, then three conversions: read(), a nearest sampler, a linear sampler
static void check_frame(NSString *tag, CVPixelBufferRef pb, Wrap *wy, Wrap *wc, int conversions) {
    const int t = is10(CVPixelBufferGetPixelFormatType(pb));
    const size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
    const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
    NSString *why = nil;
    step("plane readback");
    id<MTLBuffer> ry = gpu_read(wy->tex, t ? gRd16 : gRd8, w, h, &why);
    id<MTLBuffer> rc = ry ? gpu_read(wc->tex, t ? gRd16 : gRd8, cw, ch, &why) : nil;
    Planes pl = lock_planes(pb);
    if (!ry || !rc) result("FAIL", [NSString stringWithFormat:@"%@ plane readback: %@", tag, why]);
    else {
        NSMutableString *f = [NSMutableString string];
        cmp_report(tag, "luma plane read back", cmp_plane(ry.contents, &pl, 0, w, h, f), w * h, f, nil);
        f = [NSMutableString string];
        cmp_report(tag, "chroma plane read back", cmp_plane(rc.contents, &pl, 1, cw, ch, f), cw * ch, f, nil);
    }
    if (conversions) {
        CFTypeRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
        P p = params(CVPixelBufferGetPixelFormatType(pb), t, m);
        if (m) CFRelease(m);
        struct { const char *name; id<MTLRenderPipelineState> ps; id<MTLSamplerState> s; int lin, tol; } k[3] = {
            { "YUV->RGB read()", gCvtRead, nil, 0, 1 }, { "YUV->RGB nearest sampler", gCvtNear, gNear, 0, 1 },
            { "YUV->RGB linear sampler", gCvtLin, gLin, 1, 6 } };
        for (int i = 0; i < 3; i++) {
            step(k[i].name);
            const double t0 = now_s();
            id<MTLBuffer> o = gpu_convert(k[i].ps, wy->tex, wc->tex, k[i].s, &p, w, h, &why);
            const double ms = (now_s() - t0) * 1e3;
            if (!o) { result("FAIL", [NSString stringWithFormat:@"%@ %s: %@", tag, k[i].name, why]); continue; }
            NSMutableString *f = [NSMutableString string]; int maxd = 0;
            const size_t bad = cmp_rgb(o.contents, &pl, &p, w, h, cw, ch, k[i].lin, k[i].tol, &maxd, f);
            cmp_report(tag, k[i].name, bad, w * h, f, [NSString stringWithFormat:@" (max diff %d, tolerance %d, %.1f ms)", maxd, k[i].tol, ms]);
        }
    }
    unlock_planes(pb);
}

static void synth(OSType fmt, size_t w, size_t h, int viaCache) {
    NSString *tag = [NSString stringWithFormat:@"%@ %zux%zu %s:", fourcc(fmt), w, h, viaCache ? "cvcache" : "direct"];
    step("create buffer");
    CVPixelBufferRef pb = newbuf(fmt, w, h);
    if (!pb) { result("FAIL", [NSString stringWithFormat:@"%@ CVPixelBufferCreate failed", tag]); return; }
    if (!viaCache) result("INFO", [NSString stringWithFormat:@"%@ %@", tag, geometry(pb)]);
    fill(pb, 1);
    const int t = is10(fmt);
    step(viaCache ? "CVMetalTextureCache wrap" : "newTextureWithDescriptor:iosurface:plane:");
    Wrap wy = wrap(pb, 0, yfmt(t), w, h, viaCache, MTLTextureUsageShaderRead);
    Wrap wc = wrap(pb, 1, cfmt(t), CVPixelBufferGetWidthOfPlane(pb, 1), CVPixelBufferGetHeightOfPlane(pb, 1), viaCache, MTLTextureUsageShaderRead);
    if (!wy.tex || !wc.tex) result("FAIL", [NSString stringWithFormat:@"%@ plane wrap returned nil (luma %s, chroma %s)", tag, wy.tex ? "ok" : "nil", wc.tex ? "ok" : "nil"]);
    else check_frame(tag, pb, &wy, &wc, 1);
    unwrap(&wy); unwrap(&wc); CVPixelBufferRelease(pb);
}

// the GPU writes both planes (a render target on each), the CPU reads them
static void gpuwrite(OSType fmt, size_t w, size_t h) {
    NSString *tag = [NSString stringWithFormat:@"%@ %zux%zu GPU writes planes:", fourcc(fmt), w, h];
    CVPixelBufferRef pb = newbuf(fmt, w, h);
    if (!pb) { result("FAIL", [NSString stringWithFormat:@"%@ CVPixelBufferCreate failed", tag]); return; }
    fill(pb, 99);   // something else first: a write that never lands shows as seed 99
    const int t = is10(fmt);
    const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
    step("wrap planes as render targets");
    const MTLTextureUsage u = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    Wrap wy = wrap(pb, 0, yfmt(t), w, h, 0, u), wc = wrap(pb, 1, cfmt(t), cw, ch, 0, u);
    if (!wy.tex || !wc.tex) { result("FAIL", [NSString stringWithFormat:@"%@ render-target wrap returned nil", tag]); goto out; }
    {
        P p = params(fmt, t, NULL); p.seed = 5;
        id<MTLCommandBuffer> cb = [gQ commandBuffer];
        for (int k = 0; k < 2; k++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = k ? wc.tex : wy.tex; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:k ? gWc[t] : gWy[t]]; [e setFragmentBytes:&p length:sizeof p atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
        }
        step("GPU writes into the planes");
        NSString *why = nil;
        if (!run(cb, &why)) { result("FAIL", [NSString stringWithFormat:@"%@ %@", tag, why]); goto out; }
        Planes pl = lock_planes(pb);
        size_t bad[2] = { 0, 0 }; int stale = 0; NSMutableString *f = [NSMutableString string];
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
            const uint32_t want = t ? PY((unsigned)x, (unsigned)y, 5, 1) << 6 : PY((unsigned)x, (unsigned)y, 5, 0), g = raw_y(&pl, x, y);
            if (g != want) { if (bad[0]++ < 2) [f appendFormat:@" luma (%zu,%zu) want 0x%x got 0x%x;", x, y, want, g];
                             if (g == (t ? PY((unsigned)x, (unsigned)y, 99, 1) << 6 : PY((unsigned)x, (unsigned)y, 99, 0))) stale++; }
        }
        for (size_t y = 0; y < ch; y++) for (size_t x = 0; x < cw; x++) {
            const uint32_t b = PB((unsigned)x, (unsigned)y, 5, t), r = PR((unsigned)x, (unsigned)y, 5, t);
            const uint32_t want = t ? (b << 6) | ((r << 6) << 16) : b | (r << 8), g = raw_c(&pl, x, y);
            if (g != want && bad[1]++ < 2) [f appendFormat:@" chroma (%zu,%zu) want 0x%x got 0x%x;", x, y, want, g];
        }
        unlock_planes(pb);
        if (!bad[0] && !bad[1]) result("PASS", [NSString stringWithFormat:@"%@ the CPU sees both planes the GPU wrote", tag]);
        else result("FAIL", [NSString stringWithFormat:@"%@ luma %zu/%zu wrong (%d still the CPU's old pattern), chroma %zu/%zu wrong;%@",
                             tag, bad[0], w * h, stale, bad[1], cw * ch, f]);
    }
out:
    unwrap(&wy); unwrap(&wc); CVPixelBufferRelease(pb);
}

// A decoder's pool: buffers come back with new content under the same IOSurface. mode 0: a new CVMetalTexture per frame,
// 1: a new direct wrapper per frame (CoreAnimation's way), 2: one wrapper per surface kept across frames.
static void pool(OSType fmt, size_t w, size_t h, int mode, int frames) {
    static const char *names[] = { "cvcache, new texture per frame", "direct, new wrapper per frame", "direct, wrapper kept per surface" };
    NSString *tag = [NSString stringWithFormat:@"%@ %zux%zu pool (%s):", fourcc(fmt), w, h, names[mode]];
    NSDictionary *a = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(fmt), (id)kCVPixelBufferWidthKey: @(w), (id)kCVPixelBufferHeightKey: @(h),
                         (id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferPoolRef pl = NULL;
    if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, &pl) != kCVReturnSuccess) { result("FAIL", [NSString stringWithFormat:@"%@ pool create failed", tag]); return; }
    const int t = is10(fmt);
    NSMutableArray *held = [NSMutableArray array];          // a player holds the frame on screen and the next one
    NSMutableDictionary *kept = [NSMutableDictionary dictionary];
    NSMutableSet *ids = [NSMutableSet set];
    int badFrames = 0; NSMutableString *f = [NSMutableString string]; double ms = 0;
    for (int i = 0; i < frames; i++) {
        CVPixelBufferRef pb = NULL;
        step("pool buffer");
        if (CVPixelBufferPoolCreatePixelBuffer(NULL, pl, &pb) != kCVReturnSuccess) { result("FAIL", [NSString stringWithFormat:@"%@ frame %d: no buffer", tag, i]); break; }
        const unsigned seed = (unsigned)i + 1;
        fill(pb, seed);
        NSNumber *sid = @(IOSurfaceGetID(CVPixelBufferGetIOSurface(pb))); [ids addObject:sid];
        const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
        id<MTLTexture> ty, tc; Wrap wy = { nil, NULL }, wc = { nil, NULL };
        step("wrap pool buffer");
        const double t0 = now_s();
        if (mode == 2 && kept[sid]) { ty = kept[sid][0]; tc = kept[sid][1]; }
        else {
            wy = wrap(pb, 0, yfmt(t), w, h, mode == 0, MTLTextureUsageShaderRead);
            wc = wrap(pb, 1, cfmt(t), cw, ch, mode == 0, MTLTextureUsageShaderRead);
            ty = wy.tex; tc = wc.tex;
            if (mode == 2 && ty && tc) kept[sid] = @[ty, tc];
        }
        if (!ty || !tc) { badFrames++; [f appendFormat:@" frame %d: wrap nil;", i]; unwrap(&wy); unwrap(&wc); CVPixelBufferRelease(pb); continue; }
        NSString *why = nil;
        step("read back pool frame");
        id<MTLBuffer> ry = gpu_read(ty, t ? gRd16 : gRd8, w, h, &why), rc = ry ? gpu_read(tc, t ? gRd16 : gRd8, cw, ch, &why) : nil;
        ms += (now_s() - t0) * 1e3;
        if (!ry || !rc) { badFrames++; [f appendFormat:@" frame %d: %@;", i, why]; }
        else {
            Planes p = lock_planes(pb); NSMutableString *x = [NSMutableString string];
            const size_t by = cmp_plane(ry.contents, &p, 0, w, h, x), bc = cmp_plane(rc.contents, &p, 1, cw, ch, x);
            unlock_planes(pb);
            if (by || bc) {
                // which frame does the GPU show? the pattern of an earlier seed at a few points
                int shows = -1; const uint32_t *g = ry.contents;
                for (int s = (int)seed - 1; s >= 0 && s >= (int)seed - 8 && shows < 0; s--) {
                    int ok = 1;
                    for (size_t k = 0; k < 64 && ok; k++) { size_t xx = (k * 977) % w, yy = (k * 389) % h;
                        uint32_t want = t ? PY((unsigned)xx, (unsigned)yy, (unsigned)s, 1) << 6 : PY((unsigned)xx, (unsigned)yy, (unsigned)s, 0);
                        if (g[yy * w + xx] != want) ok = 0; }
                    if (ok) shows = s;
                }
                if (badFrames++ < 4) [f appendFormat:@" frame %d (surface %@): luma %zu chroma %zu wrong%@;", i, sid, by, bc,
                                      shows >= 0 ? [NSString stringWithFormat:@", shows frame %d (stale)", shows - 1] : @""];
            }
        }
        unwrap(&wy); unwrap(&wc);
        [held addObject:(__bridge_transfer id)pb];
        if (held.count > 2) [held removeObjectAtIndex:0];
        if (mode == 0 && i % 4 == 3) CVMetalTextureCacheFlush(gCache, 0);
    }
    [held removeAllObjects]; [kept removeAllObjects];
    CVPixelBufferPoolRelease(pl);
    NSString *sum = [NSString stringWithFormat:@" %d frames over %lu surfaces, %.2f ms per frame wrap+readback", frames, (unsigned long)ids.count, ms / frames];
    if (!badFrames) result("PASS", [tag stringByAppendingString:sum]);
    else result("FAIL", [NSString stringWithFormat:@"%@%@: %d bad frames;%@", tag, sum, badFrames, f]);
}

// packed 4:2:2: the two Metal formats made for it, and the usual way round them (RGBA8 at half width)
static void packed(OSType fmt, MTLPixelFormat mf, const char *mfname) {
    const size_t w = 1920, h = 1080;
    NSString *tag = [NSString stringWithFormat:@"%@ %zux%zu:", fourcc(fmt), w, h];
    CVPixelBufferRef pb = newbuf(fmt, w, h);
    if (!pb) { result("INFO", [NSString stringWithFormat:@"%@ CoreVideo would not create it", tag]); return; }
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *b = CVPixelBufferGetBaseAddress(pb); const size_t r = CVPixelBufferGetBytesPerRow(pb);
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w * 2; x++) b[y * r + x] = (uint8_t)(x * 3 + y * 5 + (x >> 7));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    step("packed 4:2:2 wrap");
    Wrap n = wrap(pb, 0, mf, w, h, 0, MTLTextureUsageShaderRead);
    if (!n.tex) result("GAP", [NSString stringWithFormat:@"%@ MTLPixelFormat%s: nil (the plugin does not carry this format)", tag, mfname]);
    else result("PASS", [NSString stringWithFormat:@"%@ MTLPixelFormat%s texture created", tag, mfname]);
    unwrap(&n);
    Wrap q = wrap(pb, 0, MTLPixelFormatRGBA8Unorm, w / 2, h, 0, MTLTextureUsageShaderRead);
    if (!q.tex) { result("FAIL", [NSString stringWithFormat:@"%@ RGBA8 at half width: nil", tag]); CVPixelBufferRelease(pb); return; }
    NSString *why = nil;
    step("packed 4:2:2 as RGBA8 readback");
    id<MTLBuffer> o = gpu_read(q.tex, gRd32, w / 2, h, &why);
    if (!o) result("FAIL", [NSString stringWithFormat:@"%@ RGBA8 at half width: %@", tag, why]);
    else {
        size_t bad = 0; NSMutableString *f = [NSMutableString string];
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w / 2; x++) {
            const uint32_t want = *(const uint32_t *)(b + y * r + x * 4), g = ((const uint32_t *)o.contents)[y * (w / 2) + x];
            if (g != want && bad++ < 3) [f appendFormat:@" (%zu,%zu) want 0x%08x got 0x%08x;", x, y, want, g];
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        cmp_report(tag, "as RGBA8 at half width read back", bad, w / 2 * h, f, nil);
    }
    unwrap(&q); CVPixelBufferRelease(pb);
}

static void bgra_sanity(void) {
    const size_t w = 1920, h = 1080;
    CVPixelBufferRef pb = newbuf(kCVPixelFormatType_32BGRA, w, h);
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *b = CVPixelBufferGetBaseAddress(pb); const size_t r = CVPixelBufferGetBytesPerRow(pb);
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w * 4; x++) b[y * r + x] = (uint8_t)(x * 7 + y * 3);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    step("BGRA wrap");
    Wrap q = wrap(pb, 0, MTLPixelFormatBGRA8Unorm, w, h, 0, MTLTextureUsageShaderRead);
    NSString *why = nil;
    id<MTLBuffer> o = q.tex ? gpu_read(q.tex, gRd32, w, h, &why) : nil;
    if (!o) result("FAIL", [NSString stringWithFormat:@"BGRA 1920x1080 (harness check): %@", q.tex ? why : @"wrap nil"]);
    else {
        size_t bad = 0; NSMutableString *f = [NSMutableString string];
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
            const uint8_t *p = b + y * r + x * 4;
            const uint32_t want = p[2] | (p[1] << 8) | (p[0] << 16) | ((uint32_t)p[3] << 24), g = ((const uint32_t *)o.contents)[y * w + x];
            if (g != want && bad++ < 3) [f appendFormat:@" (%zu,%zu) want 0x%08x got 0x%08x;", x, y, want, g];
        }
        cmp_report(@"BGRA 1920x1080 (harness check):", "read back", bad, w * h, f, nil);
    }
    unwrap(&q); CVPixelBufferRelease(pb);
}

static id<MTLRenderPipelineState> mkrp(id<MTLLibrary> lib, NSString *fs, MTLPixelFormat f) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = f;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [gDev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { printf("FAIL render pipeline %s: %s\n", fs.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    return ps;
}
static id<MTLComputePipelineState> mkcp(id<MTLLibrary> lib, NSString *fn) {
    NSError *e = nil; id<MTLComputePipelineState> ps = [gDev newComputePipelineStateWithFunction:[lib newFunctionWithName:fn] error:&e];
    if (!ps) { printf("FAIL compute pipeline %s: %s\n", fn.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    return ps;
}
static void setup(void) {
    step("MTLCreateSystemDefaultDevice");
    gDev = MTLCreateSystemDefaultDevice();
    if (!gDev) { printf("FAIL no Metal device (is this process allow-listed? NVMTL_ENABLE=1)\n"); exit(1); }
    result("INFO", [NSString stringWithFormat:@"device %@ (%s)", gDev.name, object_getClassName(gDev)]);
    gQ = [gDev newCommandQueue];
    step("compile shaders");
    NSError *e = nil; id<MTLLibrary> lib = [gDev newLibraryWithSource:kSrc options:nil error:&e];
    if (!lib) { printf("FAIL shader library: %s\n", e.localizedDescription.UTF8String); exit(1); }
    step("pipelines");
    gRd8 = mkcp(lib, @"rd8"); gRd16 = mkcp(lib, @"rd16"); gRd32 = mkcp(lib, @"rd32");
    gCvtRead = mkrp(lib, @"fs_read", MTLPixelFormatBGRA8Unorm); gCvtNear = mkrp(lib, @"fs_near", MTLPixelFormatBGRA8Unorm);
    gCvtLin = mkrp(lib, @"fs_lin", MTLPixelFormatBGRA8Unorm);
    gWy[0] = mkrp(lib, @"fs_wy", MTLPixelFormatR8Unorm); gWc[0] = mkrp(lib, @"fs_wc", MTLPixelFormatRG8Unorm);
    gWy[1] = mkrp(lib, @"fs_wy", MTLPixelFormatR16Unorm); gWc[1] = mkrp(lib, @"fs_wc", MTLPixelFormatRG16Unorm);
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    s.sAddressMode = s.tAddressMode = MTLSamplerAddressModeClampToEdge; s.normalizedCoordinates = YES;
    s.minFilter = s.magFilter = MTLSamplerMinMagFilterNearest; gNear = [gDev newSamplerStateWithDescriptor:s];
    s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear; gLin = [gDev newSamplerStateWithDescriptor:s];
    if (CVMetalTextureCacheCreate(NULL, NULL, gDev, NULL, &gCache) != kCVReturnSuccess) { printf("FAIL CVMetalTextureCacheCreate\n"); exit(1); }
}

static void synthetic(int quick) {
    bgra_sanity();
    const OSType fmts[] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange };
    const size_t sizes[][2] = { { 1920, 1080 }, { 854, 480 }, { 3840, 2160 } };
    for (int f = 0; f < (quick ? 1 : 3); f++)
        for (int s = 0; s < (quick ? 1 : 3); s++)
            for (int c = 0; c < 2; c++) synth(fmts[f], sizes[s][0], sizes[s][1], c);
    gpuwrite(fmts[0], 1920, 1080);
    if (!quick) gpuwrite(fmts[2], 1920, 1080);
    for (int m = 0; m < 3; m++) pool(fmts[0], 1920, 1080, m, 40);
    if (!quick) pool(fmts[2], 3840, 2160, 1, 20);
    packed(kCVPixelFormatType_422YpCbCr8, MTLPixelFormatBGRG422, "BGRG422");
    packed(kCVPixelFormatType_422YpCbCr8_yuvs, MTLPixelFormatGBGR422, "GBGR422");
}

static void vt(void) {
    result("INFO", [NSString stringWithFormat:@"VideoToolbox hardware decode: H.264 %s, HEVC %s",
                    VTIsHardwareDecodeSupported(kCMVideoCodecType_H264) ? "yes" : "no", VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC) ? "yes" : "no"]);
}

// frames decoded by VideoToolbox, as QuickTime gets them
static void asset(const char *path, int frames) {
    AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path)] options:nil];
    step("load asset");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AVAssetTrack *tr = [a tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (!tr) { result("FAIL", [NSString stringWithFormat:@"asset %s: no video track", path]); return; }
    CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)tr.formatDescriptions.firstObject;
#pragma clang diagnostic pop
    int bits = 8;
    CFNumberRef bpc = fd ? CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_BitsPerComponent) : NULL;
    if (bpc && CFGetTypeID(bpc) == CFNumberGetTypeID()) CFNumberGetValue(bpc, kCFNumberIntType, &bits);
    const OSType fmt = bits > 8 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const FourCharCode codec = fd ? CMFormatDescriptionGetMediaSubType(fd) : 0;
    NSString *tag = [NSString stringWithFormat:@"%s (%@, %d-bit) as %@:", path, fourcc(codec), bits, fourcc(fmt)];
    NSError *e = nil;
    AVAssetReader *r1 = [AVAssetReader assetReaderWithAsset:a error:&e], *r2 = [AVAssetReader assetReaderWithAsset:a error:&e];
    NSDictionary *o1 = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(fmt), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    NSDictionary *o2 = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
    AVAssetReaderTrackOutput *t1 = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr outputSettings:o1];
    AVAssetReaderTrackOutput *t2 = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr outputSettings:o2];
    t1.alwaysCopiesSampleData = NO; t2.alwaysCopiesSampleData = NO;
    if (!r1 || !r2 || ![r1 canAddOutput:t1] || ![r2 canAddOutput:t2]) { result("FAIL", [NSString stringWithFormat:@"%@ reader: %@", tag, e]); return; }
    [r1 addOutput:t1]; [r2 addOutput:t2];
    step("start reading"); [r1 startReading]; [r2 startReading];
    NSMutableSet *ids = [NSMutableSet set];
    int n = 0, badPlanes = 0, badRGB = 0, worstRGB = 0; double vtSum = 0; long vtN = 0; int vtMax = 0; double ms = 0, dec = 0;
    NSMutableString *f = [NSMutableString string];
    while (n < frames) {
        step("decode a frame (VideoToolbox)");
        const double d0 = now_s();
        CMSampleBufferRef s1 = [t1 copyNextSampleBuffer];
        dec += now_s() - d0;
        if (!s1) break;
        CMSampleBufferRef s2 = [t2 copyNextSampleBuffer];
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(s1), pb2 = s2 ? CMSampleBufferGetImageBuffer(s2) : NULL;
        if (!pb) { CFRelease(s1); if (s2) CFRelease(s2); continue; }
        if (n < 2) result("INFO", [NSString stringWithFormat:@"%@ frame %d %@", tag, n, geometry(pb)]);
        if (CVPixelBufferGetIOSurface(pb)) [ids addObject:@(IOSurfaceGetID(CVPixelBufferGetIOSurface(pb)))];
        const int t = is10(CVPixelBufferGetPixelFormatType(pb));
        const size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
        const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
        step("wrap a decoded frame");
        const double t0 = now_s();
        Wrap wy = wrap(pb, 0, yfmt(t), w, h, 1, MTLTextureUsageShaderRead), wc = wrap(pb, 1, cfmt(t), cw, ch, 1, MTLTextureUsageShaderRead);
        NSString *why = nil;
        id<MTLBuffer> ry = nil, rc = nil, cv = nil;
        CFTypeRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
        P p = params(CVPixelBufferGetPixelFormatType(pb), t, m);
        if (m) CFRelease(m);
        if (wy.tex && wc.tex) {
            step("read back a decoded frame");
            ry = gpu_read(wy.tex, t ? gRd16 : gRd8, w, h, &why);
            rc = ry ? gpu_read(wc.tex, t ? gRd16 : gRd8, cw, ch, &why) : nil;
            cv = rc ? gpu_convert(gCvtRead, wy.tex, wc.tex, nil, &p, w, h, &why) : nil;
        } else why = @"wrap returned nil";
        ms += (now_s() - t0) * 1e3;
        Planes pl = lock_planes(pb);
        if (!ry || !rc || !cv) { if (badPlanes++ < 3) [f appendFormat:@" frame %d: %@;", n, why]; }
        else {
            NSMutableString *x = [NSMutableString string]; int maxd = 0;
            const size_t by = cmp_plane(ry.contents, &pl, 0, w, h, x), bc = cmp_plane(rc.contents, &pl, 1, cw, ch, x);
            if (by || bc) { if (badPlanes++ < 3) [f appendFormat:@" frame %d planes: luma %zu chroma %zu wrong%@", n, by, bc, x]; }
            const size_t bg = cmp_rgb(cv.contents, &pl, &p, w, h, cw, ch, 0, 1, &maxd, x);
            if (maxd > worstRGB) worstRGB = maxd;
            if (bg && badRGB++ < 3) [f appendFormat:@" frame %d YUV->RGB: %zu wrong%@", n, bg, x];
        }
        // VideoToolbox's own BGRA of the same frame against the CPU's conversion (its matrix and rounding may differ a
        // little; a broken conversion is far off)
        if (pb2 && CVPixelBufferGetWidth(pb2) == w && CVPixelBufferGetHeight(pb2) == h) {
            CVPixelBufferLockBaseAddress(pb2, kCVPixelBufferLock_ReadOnly);
            const uint8_t *b = CVPixelBufferGetBaseAddress(pb2); const size_t br = CVPixelBufferGetBytesPerRow(pb2);
            for (size_t y = 1; y < h; y += 7) for (size_t x = 1; x < w; x += 7) {
                double rgb[3]; cvt_cpu(&p, ly(&pl, x, y), lc(&pl, x / 2, y / 2, 0), lc(&pl, x / 2, y / 2, 1), rgb);
                const uint8_t *px = b + y * br + x * 4;
                for (int i = 0; i < 3; i++) { int dd = abs(px[2 - i] - (int)lrint(rgb[i])); vtSum += dd; vtN++; if (dd > vtMax) vtMax = dd; }
            }
            CVPixelBufferUnlockBaseAddress(pb2, kCVPixelBufferLock_ReadOnly);
        }
        unlock_planes(pb);
        unwrap(&wy); unwrap(&wc);
        CFRelease(s1); if (s2) CFRelease(s2);
        if (++n % 8 == 0) CVMetalTextureCacheFlush(gCache, 0);
    }
    if (r1.status == AVAssetReaderStatusFailed) result("FAIL", [NSString stringWithFormat:@"%@ decoding failed: %@", tag, r1.error]);
    if (!n) { result("FAIL", [NSString stringWithFormat:@"%@ no frames decoded", tag]); return; }
    NSString *sum = [NSString stringWithFormat:@" %d frames over %lu surfaces, decode %.1f ms, wrap+check %.1f ms per frame",
                     n, (unsigned long)ids.count, dec * 1e3 / n, ms / n];
    if (!badPlanes && !badRGB) result("PASS", [NSString stringWithFormat:@"%@ decoded frames read back and converted exactly (max diff %d)%@", tag, worstRGB, sum]);
    else result("FAIL", [NSString stringWithFormat:@"%@ %d frames with wrong planes, %d with a wrong conversion;%@%@", tag, badPlanes, badRGB, f, sum]);
    if (vtN) {
        const double mean = vtSum / vtN;
        result(mean > 8 ? "FAIL" : "INFO", [NSString stringWithFormat:@"%@ VideoToolbox's own BGRA against the CPU's conversion: mean diff %.2f, max %d%s",
                                            tag, mean, vtMax, mean > 8 ? " (far off: VT's converter output is wrong)" : ""]);
    }
}

// ---- CoreAudio: AVPlayer times a file with sound by the output device's clock, so a device that is published but
// never runs (a codec AppleHDA cannot drive) holds the video back as well
static UInt32 ca_u32(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc, UInt32 dflt) {
    AudioObjectPropertyAddress a = { sel, sc, kAudioObjectPropertyElementMain }; UInt32 v = dflt, n = sizeof v;
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &n, &v) == noErr ? v : dflt;
}
static NSString *ca_str(AudioObjectID o, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef v = NULL; UInt32 n = sizeof v;
    if (AudioObjectGetPropertyData(o, &a, 0, NULL, &n, &v) != noErr || !v) return @"?";
    return (__bridge_transfer NSString *)v;
}
static UInt32 ca_channels(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc, kAudioObjectPropertyElementMain }; UInt32 n = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &n) != noErr || !n) return 0;
    AudioBufferList *l = malloc(n); UInt32 ch = 0;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &n, l) == noErr) for (UInt32 i = 0; i < l->mNumberBuffers; i++) ch += l->mBuffers[i].mNumberChannels;
    free(l); return ch;
}
static AudioObjectID ca_default_out(void) { return ca_u32(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, 0); }
static NSString *ca_dev(AudioObjectID d) {
    AudioObjectPropertyAddress r = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 rate = 0; UInt32 n = sizeof rate; AudioObjectGetPropertyData(d, &r, 0, NULL, &n, &rate);
    const UInt32 tr = ca_u32(d, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, 0);
    char t[5] = { (char)(tr >> 24), (char)(tr >> 16), (char)(tr >> 8), (char)tr, 0 };
    return [NSString stringWithFormat:@"device %u \"%@\" (%@, uid %@, transport '%s'): out %u ch, in %u ch, %.0f Hz, alive %u, running %u, running somewhere %u",
            d, ca_str(d, kAudioObjectPropertyName), ca_str(d, kAudioObjectPropertyManufacturer), ca_str(d, kAudioDevicePropertyDeviceUID), t,
            ca_channels(d, kAudioObjectPropertyScopeOutput), ca_channels(d, kAudioObjectPropertyScopeInput), rate,
            ca_u32(d, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, 0),
            ca_u32(d, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, 0),
            ca_u32(d, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, 0)];
}
static void audio(void) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 n = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &n);
    AudioObjectID *ids = n ? malloc(n) : NULL;
    if (ids) AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &n, ids);
    const AudioObjectID def = ca_default_out();
    result("INFO", [NSString stringWithFormat:@"audio: %u devices, default output %u%s", (unsigned)(n / sizeof(AudioObjectID)), def,
                    def ? "" : " (none: AVPlayer has no audio clock to wait for)"]);
    for (UInt32 i = 0; i < n / sizeof(AudioObjectID); i++)
        result("INFO", [NSString stringWithFormat:@"audio: %@%s", ca_dev(ids[i]), ids[i] == def ? "  <- default output" : ""]);
    free(ids);
}

// QuickTime's path: AVPlayerLayer in a window; the frames go through CoreAnimation to WindowServer
static void play(const char *path, double secs, int noaudio) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, 1280, 720)
                                              styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.title = [NSString stringWithFormat:@"yuvprobe play %s", path];
    w.contentView.wantsLayer = YES;
    w.contentView.layer.backgroundColor = CGColorCreateGenericRGB(1, 0, 1, 1);   // magenta behind the video: a hole shows
    AVPlayer *pl;
    if (noaudio) {   // the video track alone: nothing to wait for on the audio device
        AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path)] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack *vt = [a tracksWithMediaType:AVMediaTypeVideo].firstObject;
        AVMutableComposition *c = [AVMutableComposition composition];
        AVMutableCompositionTrack *ct = [c addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
        NSError *e = nil;
        if (!vt || ![ct insertTimeRange:CMTimeRangeMake(kCMTimeZero, a.duration) ofTrack:vt atTime:kCMTimeZero error:&e]) {
            result("FAIL", [NSString stringWithFormat:@"play %s noaudio: no video track to compose (%@)", path, e]); return; }
        ct.preferredTransform = vt.preferredTransform;
#pragma clang diagnostic pop
        pl = [AVPlayer playerWithPlayerItem:[AVPlayerItem playerItemWithAsset:c]];
    } else pl = [AVPlayer playerWithURL:[NSURL fileURLWithPath:@(path)]];
    const AudioObjectID dev = ca_default_out();
    printf("INFO play %s%s; default audio output: %s\n", path, noaudio ? " (video track only)" : "", dev ? ca_dev(dev).UTF8String : "none");
    AVPlayerLayer *L = [AVPlayerLayer playerLayerWithPlayer:pl];
    L.frame = w.contentView.bounds; L.videoGravity = AVLayerVideoGravityResizeAspect;
    [w.contentView.layer addSublayer:L];
    [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    [pl play];
    const double t0 = now_s(); double last = -1, nextLog = 0; int ready = 0;
    while (now_s() - t0 < secs) {
        step("play: run loop");
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05] inMode:NSDefaultRunLoopMode dequeue:YES];
        if (e) [NSApp sendEvent:e];
        if (now_s() - t0 >= nextLog) {
            nextLog += 1;
            const double ct = CMTimeGetSeconds(pl.currentTime);
            if (L.readyForDisplay) ready = 1;
            static const char *tcs[] = { "paused", "waiting", "playing" };
            const long tc = (long)pl.timeControlStatus;
            printf("INFO play %.0f s: clock %.2f s, rate %.1f, %s%s%s, item status %ld, layer readyForDisplay %d, audio device running %u%s%s\n",
                   now_s() - t0, ct, pl.rate, tc >= 0 && tc <= 2 ? tcs[tc] : "?", pl.reasonForWaitingToPlay ? " because " : "",
                   pl.reasonForWaitingToPlay ? pl.reasonForWaitingToPlay.UTF8String : "", (long)pl.currentItem.status, L.readyForDisplay,
                   dev ? ca_u32(dev, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, 0) : 0,
                   pl.currentItem.error ? ", error " : "", pl.currentItem.error ? pl.currentItem.error.localizedDescription.UTF8String : "");
            fflush(stdout);
            last = ct;
        }
    }
    const double ran = CMTimeGetSeconds(pl.currentTime);
    [pl pause];
    NSString *msg = [NSString stringWithFormat:@"play %s%s: the player's clock ran %.1f s of %.0f, layer ready for display %s", path,
                     noaudio ? " (video only)" : "", ran, secs, ready ? "yes" : "never"];
    result(ran > secs * 0.6 && ready ? "PASS" : "FAIL", msg);
    (void)last;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL);
        const char *cmd = argc > 1 ? argv[1] : "all";
        if (!strcmp(cmd, "vt")) { vt(); return 0; }
        if (!strcmp(cmd, "audio")) { audio(); return 0; }
        if (!strcmp(cmd, "play")) {
            if (argc < 3) { fprintf(stderr, "usage: yuvprobe play <file> [seconds]\n"); return 2; }
            play(argv[2], argc > 3 ? atof(argv[3]) : 8, argc > 4 && !strcmp(argv[4], "noaudio"));
        } else {
            setup();
            if (!strcmp(cmd, "asset")) {
                if (argc < 3) { fprintf(stderr, "usage: yuvprobe asset <file> [frames]\n"); return 2; }
                asset(argv[2], argc > 3 ? atoi(argv[3]) : 60);
            } else if (!strcmp(cmd, "quick")) synthetic(1);
            else if (!strcmp(cmd, "all")) synthetic(0);
            else { fprintf(stderr, "usage: yuvprobe [all | quick | vt | audio | asset <file> [frames] | play <file> [s] [noaudio]]\n"); return 2; }
        }
        printf("yuvprobe: %d passed, %d failed, %d format gaps\n", gPass, gFail, gGap);
        return gFail ? 1 : 0;
    }
}
