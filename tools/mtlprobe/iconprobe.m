// Renders application icons in this process (IconServices' ISIcon, which draws Tahoe's layered icons with RenderBox on
// the Metal device when one exists) into a PNG grid, to compare the GPU path against the expected artwork.
// Usage: iconprobe out.png [bundle ids...]
#import <AppKit/AppKit.h>
#import <objc/message.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: iconprobe out.png [bundle ids]\n"); return 2; }
        dlopen("/System/Library/PrivateFrameworks/IconServices.framework/IconServices", RTLD_NOW);
        NSMutableArray *ids = [NSMutableArray array];
        for (int i = 2; i < argc; i++) [ids addObject:@(argv[i])];
        if (!ids.count) [ids addObjectsFromArray:@[ @"com.apple.AppStore", @"com.apple.TV", @"com.apple.freeform", @"com.apple.Preview",
                                                   @"com.apple.Photos", @"com.apple.weather", @"com.apple.iCal", @"com.apple.Music" ]];
        const int S = 128, n = (int)ids.count;
        NSBitmapImageRep *grid = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:S * n pixelsHigh:S * 2
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:grid];
        [[NSColor colorWithWhite:0.5 alpha:1] setFill]; NSRectFill(NSMakeRect(0, 0, S * n, S * 2));
        Class ISBundleIcon = NSClassFromString(@"ISBundleIcon"), ISImageDescriptor = NSClassFromString(@"ISImageDescriptor");
        for (int i = 0; i < n; i++) {
            // row 1 (top): NSWorkspace (IconServices' store, filled by whichever process drew it); row 2: generated here
            NSURL *url = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:ids[i]];
            NSImage *im = url ? [[NSWorkspace sharedWorkspace] iconForFile:url.path] : nil;
            [im drawInRect:NSMakeRect(i * S, S, S, S)];
            CGImageRef cg = NULL;
            if (url && ISBundleIcon && ISImageDescriptor) {
                id icon = ((id (*)(id, SEL, id))objc_msgSend)([ISBundleIcon alloc], NSSelectorFromString(@"initWithBundleURL:"), url);
                id desc = ((id (*)(id, SEL, CGSize, CGFloat))objc_msgSend)([ISImageDescriptor alloc], NSSelectorFromString(@"initWithSize:scale:"), CGSizeMake(S, S), 1.0);
                id img = icon ? ((id (*)(id, SEL, id))objc_msgSend)(icon, NSSelectorFromString(@"generateImageWithDescriptor:"), desc) : nil;
                if (img && [img respondsToSelector:NSSelectorFromString(@"CGImage")]) cg = ((CGImageRef (*)(id, SEL))objc_msgSend)(img, NSSelectorFromString(@"CGImage"));
                if (cg) CGContextDrawImage(NSGraphicsContext.currentContext.CGContext, CGRectMake(i * S, 0, S, S), cg);
            }
            printf("%-22s workspace %s, generated %s\n", [ids[i] UTF8String], im ? "ok" : "none", cg ? "ok" : "none");
        }
        [NSGraphicsContext restoreGraphicsState];
        [[grid representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES];
    }
    return 0;
}
