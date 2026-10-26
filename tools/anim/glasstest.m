// Liquid Glass outside the Dock: a window of black-and-white stripes with Tahoe's NSGlassEffectView in both styles
// (0 regular, 1 clear) and the older NSVisualEffectView over them. On a GPU that renders the glass, stripes bend near the
// glass edges (lensing); a plain blur keeps them straight and only smears them. Shown for argv[1] seconds.
// Run in the logged-in GUI session: launchctl asuser <uid> ./glasstest 20 (window at x 200, y 300 points, 1240 x 460)
#import <AppKit/AppKit.h>
#import <objc/message.h>

@interface Stripes : NSView
@end
@implementation Stripes
- (void)drawRect:(NSRect)r {
    if (getenv("GLASS_CHECKER")) {   // 64-point checkerboard, coloured discs and text: low frequencies a moderate blur keeps
        for (int y = 0; y * 64 < self.bounds.size.height; y++) for (int x = 0; x * 64 < self.bounds.size.width; x++) {
            [((x + y) & 1) ? [NSColor whiteColor] : [NSColor colorWithWhite:0.15 alpha:1] setFill]; NSRectFill(NSMakeRect(x * 64, y * 64, 64, 64)); }
        NSArray *cs = @[[NSColor systemRedColor], [NSColor systemGreenColor], [NSColor systemBlueColor], [NSColor systemYellowColor]];
        for (int i = 0; i < 12; i++) { [cs[i % 4] setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(20 + i * 100, 150, 70, 70)] fill]; }
        [@"Liquid Glass lensing test 0123456789" drawAtPoint:NSMakePoint(30, 260) withAttributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:44],
            NSForegroundColorAttributeName: [NSColor systemPurpleColor] }];
        return;
    }
    [[NSColor whiteColor] setFill]; NSRectFill(self.bounds);
    [[NSColor blackColor] setFill];
    for (CGFloat x = 0; x < self.bounds.size.width; x += 16) NSRectFill(NSMakeRect(x, 0, 8, self.bounds.size.height));
    [[NSColor systemRedColor] setFill];
    for (CGFloat y = 0; y < self.bounds.size.height; y += 40) NSRectFill(NSMakeRect(0, y, self.bounds.size.width, 4));
}
@end

static NSView *glass(NSInteger style, NSRect f)
{
    Class c = NSClassFromString(@"NSGlassEffectView");
    NSView *g = [[c alloc] initWithFrame:f];
    ((void (*)(id, SEL, NSInteger))objc_msgSend)(g, NSSelectorFromString(@"setStyle:"), style);
    ((void (*)(id, SEL, CGFloat))objc_msgSend)(g, NSSelectorFromString(@"setCornerRadius:"), 40);
    return g;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 20;
        [NSApplication sharedApplication];
        const BOOL active = getenv("GLASS_ACTIVE") != NULL;   // a key window of a regular, active app instead of a floating accessory
        [NSApp setActivationPolicy:active ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 300, 1240, 460)
                                                  styleMask:active ? NSWindowStyleMaskTitled | NSWindowStyleMaskFullSizeContentView : NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered defer:NO];
        if (active) { w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden; }
        w.level = NSFloatingWindowLevel;
        Stripes *s = [[Stripes alloc] initWithFrame:NSMakeRect(0, 0, 1240, 460)];
        w.contentView = s;
        [s addSubview:glass(0, NSMakeRect(40, 120, 340, 220))];
        [s addSubview:glass(1, NSMakeRect(450, 120, 340, 220))];
        NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(860, 120, 340, 220)];
        v.material = NSVisualEffectMaterialHUDWindow; v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
        v.state = NSVisualEffectStateActive; v.wantsLayer = YES; v.layer.cornerRadius = 40; v.layer.masksToBounds = YES;
        [s addSubview:v];
        if (active) { [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; } else [w orderFrontRegardless];
        if (getenv("GLASS_MOVE")) {   // the panels slide back and forth, so every frame redraws the glass
            NSArray<NSView *> *panels = [s.subviews copy]; __block double t = 0;
            [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *tm) {
                t += 1.0 / 120; const CGFloat dx = 30 * sin(t * 3);
                for (NSUInteger i = 0; i < panels.count; i++) [panels[i] setFrameOrigin:NSMakePoint(40 + 410 * i + dx, 120)]; }];
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        [NSApp run];
    }
    return 0;
}
