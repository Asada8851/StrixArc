// Times NullMoth's AIR->SPIR-V translator on one cached AIR text: the plain translation (no function constant values)
// and the specialization with values (what a new compositor variant costs WindowServer), and counts the specialization
// constants (OpSpecConstant*, SpecId) the plain SPIR-V keeps, which RADV could specialize itself.
// Usage: xlateprobe <file.ll> <stage> [rounds]
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*xl_fn)(const char *, const char *, uint8_t **, size_t *, char *, size_t);
typedef int (*fc_fn)(const char *, const char *, const uint32_t *, const uint32_t *, const uint8_t *const *, size_t, uint8_t **, size_t *, char *, size_t);

static double ms(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6; }

static void census(const uint8_t *spv, size_t len)
{
    const uint32_t *w = (const uint32_t *)spv; size_t n = len / 4, specs = 0, specid = 0, fns = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc) break;
        if (op >= 48 && op <= 52) specs++;                 // OpSpecConstantTrue/False/ /Composite/Op
        if (op == 71 && i + 2 < n && w[i + 2] == 1) specid++;   // OpDecorate SpecId
        if (op == 54) fns++;                                // OpFunction
        i += wc;
    }
    printf("  %zu bytes, %zu OpSpecConstant*, %zu SpecId decorations, %zu functions\n", len, specs, specid, fns);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: xlateprobe file.ll stage [rounds]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *ll = malloc(sz + 1); fread(ll, 1, sz, f); ll[sz] = 0; fclose(f);
    void *h = dlopen("/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    xl_fn xl = (xl_fn)dlsym(h, "nvmtl_translate"); fc_fn fc = (fc_fn)dlsym(h, "nvmtl_translate_fc");
    void (*setcaps)(uint32_t) = (void (*)(uint32_t))dlsym(h, "nvmtl_translate_set_caps"); if (setcaps) setcaps(3u);
    char err[512]; uint8_t *out = NULL; size_t len = 0;
    double t = ms(); int r = xl(ll, argv[2], &out, &len, err, sizeof err);
    printf("plain translate: rc %d, %.1f ms%s%s\n", r, ms() - t, r ? " - " : "", r ? err : "");
    if (!r) census(out, len);
    // the function constants: "!air.function_constant" globals are numbered in the order the metadata lists them;
    // give every bool index 0..63 a value from a seed (the translator ignores indices the function does not declare)
    const int rounds = argc > 3 ? atoi(argv[3]) : 1;
    for (int k = 0; k < rounds; k++) {
        uint32_t idx[64], sizes[64]; uint8_t vals[64]; const uint8_t *pay[64]; unsigned s = 1234u + k;
        for (int i = 0; i < 64; i++) { idx[i] = i; sizes[i] = 1; s = s * 1103515245u + 12345u; vals[i] = (s >> 16) & 1; pay[i] = &vals[i]; }
        out = NULL; len = 0; t = ms();
        r = fc(ll, argv[2], idx, sizes, pay, 64, &out, &len, err, sizeof err);
        printf("specialized translate %d: rc %d, %.1f ms%s%s\n", k, r, ms() - t, r ? " - " : "", r ? err : "");
        if (!r) census(out, len);
    }
    return 0;
}
