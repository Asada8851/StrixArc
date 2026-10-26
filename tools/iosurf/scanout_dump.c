// Dumps the panel's scan-out (VRAM offset 0, 2560x1600 XRGB, pitch 10240) through PhxBringup's DBG_VRAM_READ
// (selector 102, 256 dwords a call) to a raw file: what the panel shows, for checking rendering without a camera.
// Usage (root): scanout_dump out.raw [width height pitch]
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: scanout_dump out.raw [w h pitch]\n"); return 2; }
    const unsigned W = argc > 3 ? atoi(argv[2]) : 2560, H = argc > 3 ? atoi(argv[3]) : 1600, P = argc > 4 ? atoi(argv[4]) : 10240;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    FILE *f = fopen(argv[1], "wb");
    uint32_t buf[256];
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x += 256) {
            const unsigned n = W - x < 256 ? W - x : 256;
            uint64_t in[2] = {(uint64_t)y * P + x * 4ull, n};
            size_t outsz = n * 4;
            kern_return_t kr = IOConnectCallMethod(c, 102, in, 2, NULL, 0, NULL, NULL, buf, &outsz);
            if (kr) { fprintf(stderr, "read y %u x %u: 0x%x\n", y, x, kr); return 1; }
            fwrite(buf, 4, n, f);
        }
    }
    fclose(f);
    return 0;
}
