// listmodes: what CoreGraphics knows about each display - the vendor/model numbers a display override must carry, and
// every mode macOS offers (HiDPI ones included: their pixel size is twice their point size).
//   listmodes          all displays, all modes
//   listmodes --ids    "<vendor> <model>" of the main display, decimal (for hidpi.sh)
// Build (Linux cross): clang -target x86_64-apple-macos12.0 -isysroot $SDKROOT -fuse-ld=lld -framework CoreGraphics
//   -framework CoreFoundation -o listmodes listmodes.c
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    CGDirectDisplayID ids[16];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(16, ids, &n) != kCGErrorSuccess || !n) { fprintf(stderr, "no displays\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "--ids")) {
        const CGDirectDisplayID m = CGMainDisplayID();
        printf("%u %u\n", CGDisplayVendorNumber(m), CGDisplayModelNumber(m));
        return 0;
    }
    const void *k[1] = { kCGDisplayShowDuplicateLowResolutionModes }, *v[1] = { kCFBooleanTrue };
    CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (uint32_t i = 0; i < n; i++) {
        const CGDirectDisplayID d = ids[i];
        printf("display %u: vendor %u (0x%x) model %u (0x%x) serial %u, builtin %d, main %d, %zux%zu points\n", d,
               CGDisplayVendorNumber(d), CGDisplayVendorNumber(d), CGDisplayModelNumber(d), CGDisplayModelNumber(d),
               CGDisplaySerialNumber(d), CGDisplayIsBuiltin(d), CGDisplayIsMain(d), CGDisplayPixelsWide(d), CGDisplayPixelsHigh(d));
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, opt);
        const CFIndex c = modes ? CFArrayGetCount(modes) : 0;
        printf("  %ld modes (* = current; HiDPI = pixels twice the points):\n", (long)c);
        for (CFIndex j = 0; j < c; j++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, j);
            const size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m);
            const size_t pw = CGDisplayModeGetPixelWidth(m), ph = CGDisplayModeGetPixelHeight(m);
            printf("  %c id %d  %zux%zu points, %zux%zu pixels%s, %.2f Hz, IOFlags 0x%x%s\n",
                   cur && CGDisplayModeGetIODisplayModeID(m) == CGDisplayModeGetIODisplayModeID(cur) ? '*' : ' ',
                   CGDisplayModeGetIODisplayModeID(m), w, h, pw, ph, (pw == 2 * w && ph == 2 * h) ? " HiDPI" : "",
                   CGDisplayModeGetRefreshRate(m), CGDisplayModeGetIOFlags(m),
                   CGDisplayModeIsUsableForDesktopGUI(m) ? "" : " (not for the desktop)");
        }
        if (modes) CFRelease(modes);
        if (cur) CGDisplayModeRelease(cur);
    }
    CFRelease(opt);
    return 0;
}
