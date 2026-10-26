// Times MTLBuffer creation at growing sizes (shared and private) to find where an allocation turns slow or fails.
// Usage: bufsize
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        const size_t mb[] = { 1, 2, 4, 8, 16, 24, 32, 48, 64, 128 };
        for (int s = 0; s < 2; s++)
            for (unsigned i = 0; i < sizeof mb / sizeof mb[0]; i++) {
                const double t0 = CACurrentMediaTime();
                id<MTLBuffer> b = [dev newBufferWithLength:mb[i] << 20 options:s ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared];
                printf("%-7s %4zu MB: %s in %.1f ms\n", s ? "private" : "shared", mb[i], b ? "ok" : "FAILED", (CACurrentMediaTime() - t0) * 1e3);
            }
    }
    return 0;
}
