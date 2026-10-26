// CoreAnimation's mipmapped layer images: -[MTLDevice newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:]
// over a 54x54 BGRA8 image with 6 levels (bytesPerRow 256), left half red and right half blue. Level 0 must hold the
// image and the smaller levels its average (half red, half blue), not whatever the memory held.
// "tail": a 16x16 single-level image at the end of an 8 KB buffer whose padded last row would run past it (offset
// 2576, bytesPerRow 352), as CoreAnimation hands them over; it was refused (no view, no copy).
// Usage: tiledprobe [tail]
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        const BOOL tail = argc > 1 && !strcmp(argv[1], "tail");
        const NSUInteger W = tail ? 16 : 54, H = W, bpr = tail ? 352 : 256, len = tail ? 8192 : 4 * 4096, off = tail ? 2576 : 0;
        uint8_t *mem = NULL; posix_memalign((void **)&mem, 4096, len); memset(mem, 0x5a, len);
        for (NSUInteger y = 0; y < H; y++) for (NSUInteger x = 0; x < W; x++) {
            uint8_t *p = mem + off + y * bpr + x * 4; p[0] = x < W / 2 ? 0 : 255; p[1] = 0; p[2] = x < W / 2 ? 255 : 0; p[3] = 255; }
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:!tail];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        SEL s = NSSelectorFromString(@"newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:");
        if (![(id)dev respondsToSelector:s]) { printf("device has no newTiledTextureWithBytesNoCopy\n"); return 1; }
        id<MTLTexture> t = ((id (*)(id, SEL, void *, NSUInteger, id, NSUInteger, NSUInteger))objc_msgSend)((id)dev, s, mem, len, d, off, bpr);
        if (!t) { printf("nil texture\n"); return 1; }
        printf("texture %lux%lu, %lu levels\n", (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.mipmapLevelCount);
        int bad = 0;
        for (NSUInteger l = 0; l < t.mipmapLevelCount; l++) {
            const NSUInteger w = MAX(W >> l, 1), h = MAX(H >> l, 1);
            uint8_t *o = calloc(w * h, 4);
            [t getBytes:o bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l];
            // the average over the level: red and blue near half each, alpha 255
            double r = 0, b = 0, a = 0; for (NSUInteger i = 0; i < w * h; i++) { b += o[i * 4]; r += o[i * 4 + 2]; a += o[i * 4 + 3]; }
            r /= w * h; b /= w * h; a /= w * h;
            const int ok = fabs(r - 127.5) < 20 && fabs(b - 127.5) < 20 && fabs(a - 255) < 2;
            printf("level %lu %2lux%-2lu: mean r %5.1f b %5.1f a %5.1f  %s\n", (unsigned long)l, (unsigned long)w, (unsigned long)h, r, b, a, ok ? "ok" : "WRONG");
            bad += !ok; free(o);
        }
        return bad ? 1 : 0;
    }
}
