// Samples a GPU status register (default GRBM_STATUS, absolute dword 0x2004) through PhxBringup's READREGS for N
// seconds and prints how often each bit was set. Usage (root): gpubusy [seconds] [register]
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const double secs = argc > 1 ? atof(argv[1]) : 5;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t end = mach_absolute_time() + (uint64_t)(secs * 1e9 * tb.denom / tb.numer);
    const uint64_t reg = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x2004;
    unsigned long n = 0, bit[32] = {0};
    while (mach_absolute_time() < end) {
        uint64_t in[3] = {reg, 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
        if (IOConnectCallMethod(c, 2, in, 3, NULL, 0, NULL, NULL, &v, &sz)) { fprintf(stderr, "read failed\n"); return 1; }
        n++; for (int b = 0; b < 32; b++) bit[b] += v >> b & 1;
        usleep(200);
    }
    printf("0x%llx, %lu samples; bits set:", (unsigned long long)reg, n);
    for (int b = 31; b >= 0; b--) if (bit[b]) printf(" %d:%.0f%%", b, 100.0 * bit[b] / n);
    printf("\n");
    return 0;
}
