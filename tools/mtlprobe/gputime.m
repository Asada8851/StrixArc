// GPU timestamps against the host clock: commits an empty-ish command buffer every 10 ms for N seconds and prints, for
// each one, how far GPUStartTime / GPUEndTime sit from the host time (CACurrentMediaTime) at commit and at completion.
// The compositor paces itself on these: a GPU time running ahead of the host makes it wait for work already done.
// Usage: gputime [seconds]
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 3;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        printf("device %s\n", dev.name.UTF8String);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        const double end = CACurrentMediaTime() + secs;
        int n = 0;
        while (CACurrentMediaTime() < end) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e fillBuffer:b range:NSMakeRange(0, 4096) value:(uint8_t)n];
            [e endEncoding];
            const double t0 = CACurrentMediaTime();
            [cb commit];
            [cb waitUntilCompleted];
            const double t1 = CACurrentMediaTime();
            printf("%7.3f start %+9.3f ms end %+9.3f ms (after commit / before done) gpu %.3f ms\n", t0 - (end - secs),
                   (cb.GPUStartTime - t0) * 1e3, (cb.GPUEndTime - t1) * 1e3, (cb.GPUEndTime - cb.GPUStartTime) * 1e3);
            n++;
            usleep(10000);
        }
    }
    return 0;
}
