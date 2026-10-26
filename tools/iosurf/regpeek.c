// Reads up to 16 consecutive dwords through PhxBringup's READREGS twice, a given number of ms apart, and prints both
// with the difference. Usage (root): regpeek <first dword> [count] [ms]
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static io_connect_t c;
static int rd(uint64_t reg, uint32_t n, uint32_t *v)
{
    uint64_t in[3] = {reg, n, 0xFFFFFFFFull}; size_t sz = n * 4;
    return IOConnectCallMethod(c, 2, in, 3, NULL, 0, NULL, NULL, v, &sz);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: regpeek <reg> [count] [ms]\n"); return 2; }
    const uint64_t reg = strtoull(argv[1], NULL, 0);
    const uint32_t n = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
    const int ms = argc > 3 ? atoi(argv[3]) : 100;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    uint32_t a[16], b[16];
    if (rd(reg, n, a)) { fprintf(stderr, "read failed\n"); return 1; }
    usleep(ms * 1000);
    if (rd(reg, n, b)) { fprintf(stderr, "read failed\n"); return 1; }
    for (uint32_t i = 0; i < n; i++)
        printf("0x%llx: 0x%08x -> 0x%08x  (+%u)\n", (unsigned long long)reg + i, a[i], b[i], b[i] - a[i]);
    return 0;
}
