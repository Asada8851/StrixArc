// fpsanim [seconds]: a window with a square CoreAnimation moves every 1/120 s (no implicit animations), so WindowServer
// has a new frame to composite at every refresh, whatever the panel's rate. fps_check.sh counts the frames WindowServer
// presents meanwhile (debug.nvaccel_iop_ok); this prints how many moves the app made and the refresh AppKit reports.
// Build: clang -fobjc-arc -framework Cocoa -framework QuartzCore -o fpsanim fpsanim.m
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 8.0;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(160, 160, 720, 420) styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = @"nullarc fpsanim";
        w.level = NSFloatingWindowLevel;
        NSView *v = w.contentView;
        v.wantsLayer = YES;
        v.layer.backgroundColor = CGColorCreateGenericRGB(0.08, 0.08, 0.12, 1);
        CALayer *sq = [CALayer layer];
        sq.bounds = CGRectMake(0, 0, 120, 120);
        sq.backgroundColor = CGColorCreateGenericRGB(1, 0.55, 0, 1);
        [v.layer addSublayer:sq];
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        __block unsigned long moves = 0;
        const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        NSTimer *t = [NSTimer timerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *tm) {
            const double ph = CFAbsoluteTimeGetCurrent() - t0;
            moves++;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            sq.position = CGPointMake(70 + fmod(ph * 480.0, 580.0), 210 + 120 * sin(ph * 6.0));
            [CATransaction commit];
            if (ph >= secs) {
                [tm invalidate];
                [NSApp stop:nil];
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                                   timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
            }
        }];
        [[NSRunLoop currentRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
        [NSApp run];
        const double el = CFAbsoluteTimeGetCurrent() - t0;
        long maxfps = 0;
        if (@available(macOS 12.0, *)) maxfps = (long)NSScreen.mainScreen.maximumFramesPerSecond;
        printf("fpsanim: %lu moves in %.1f s (%.1f/s); AppKit says the screen runs at %ld fps\n", moves, el, moves / el, maxfps);
    }
    return 0;
}
