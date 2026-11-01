// __cpu_model for __builtin_cpu_supports() in code cross-built for macOS without compiler-rt's libclang_rt.osx.a
// (Linux clang ships no Darwin builtins). Same layout as compiler-rt's cpu_model/x86.c: three ints, then the feature
// bits (FEATURE_CMOV = 0 ... FEATURE_SSE4_1 = 7, FEATURE_SSE4_2 = 8, FEATURE_AVX = 9, FEATURE_AVX2 = 10), filled from
// CPUID before anything in the image runs. Private to the image (hidden), as the earlier plugin builds had it.
#include <cpuid.h>
__attribute__((visibility("hidden"))) struct {
    unsigned int cpu_vendor, cpu_type, cpu_subtype;
    unsigned int cpu_features[1];
} __cpu_model;
__attribute__((constructor(101))) static void cpu_model_shim_init(void)
{
    unsigned a, b, c, d, f = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return;
    if (d & (1u << 15)) f |= 1u << 0;    // CMOV
    if (d & (1u << 23)) f |= 1u << 1;    // MMX
    if (c & (1u << 23)) f |= 1u << 2;    // POPCNT
    if (d & (1u << 25)) f |= 1u << 3;    // SSE
    if (d & (1u << 26)) f |= 1u << 4;    // SSE2
    if (c & (1u << 0))  f |= 1u << 5;    // SSE3
    if (c & (1u << 9))  f |= 1u << 6;    // SSSE3
    if (c & (1u << 19)) f |= 1u << 7;    // SSE4.1
    if (c & (1u << 20)) f |= 1u << 8;    // SSE4.2
    __cpu_model.cpu_vendor = b == 0x68747541 ? 2 : b == 0x756e6547 ? 1 : 0;   // "Auth" (AMD) / "Genu" (Intel)
    __cpu_model.cpu_features[0] = f;
}
