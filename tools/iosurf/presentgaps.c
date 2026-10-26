// Polls debug.nvaccel_iop_ok (frames WindowServer presented) and debug.phxfb_vbl_n (PhxFB's VBL ticks) every ~100 us for
// N seconds, then prints the intervals between presents and each present's delay after the last tick, in histograms.
// Usage: presentgaps [seconds]
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

static long long ctl(const char *n) { long long v = 0; size_t sz = sizeof v; sysctlbyname(n, &v, &sz, NULL, 0); return v; }

int main(int argc, char **argv)
{
    const double secs = argc > 1 ? atof(argv[1]) : 5;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const double us = (double)tb.numer / tb.denom / 1000.0;
    const uint64_t t0 = mach_absolute_time(), end = t0 + (uint64_t)(secs * 1e6 / us);
    long long f = ctl("debug.nvaccel_iop_ok"), v = ctl("debug.phxfb_vbl_n");
    uint64_t lastF = 0, lastV = 0;
    unsigned gap[64] = {0}, phase[32] = {0}, frames = 0, ticks = 0, nstall = 0;
    double stallAt[64], stallMs[64];
    for (uint64_t now; (now = mach_absolute_time()) < end; usleep(100)) {
        const long long v1 = ctl("debug.phxfb_vbl_n"), f1 = ctl("debug.nvaccel_iop_ok");
        if (v1 != v) { ticks += (unsigned)(v1 - v); v = v1; lastV = now; }
        if (f1 != f) {
            frames += (unsigned)(f1 - f); f = f1;
            if (lastF) {
                const double ms = (now - lastF) * us / 1000;
                const unsigned b = (unsigned)ms; gap[b < 63 ? b : 63]++;   // 1 ms buckets
                if (ms > 20 && nstall < 64) { stallAt[nstall] = (lastF - t0) * us / 1e6; stallMs[nstall++] = ms; }
            }
            if (lastV) { unsigned b = (unsigned)((now - lastV) * us / 500); phase[b < 31 ? b : 31]++; }   // 0.5 ms buckets
            lastF = now;
        }
    }
    printf("%u presents, %u ticks in %.1f s\npresent intervals (ms):", frames, ticks, secs);
    for (int i = 0; i < 64; i++) if (gap[i]) printf(" %d:%u", i, gap[i]);
    printf("\npresent delay after the tick (0.5 ms):");
    for (int i = 0; i < 32; i++) if (phase[i]) printf(" %.1f:%u", i / 2.0, phase[i]);
    printf("\nstalls over 20 ms (start s: length ms):");
    for (unsigned i = 0; i < nstall; i++) printf(" %.3f:%.0f", stallAt[i], stallMs[i]);
    printf("\n");
    return 0;
}
