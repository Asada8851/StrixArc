// Synthetic WindowServer load for frame-rate measurements without hands on the machine: a window that slides back and
// forth, twelve animated layers and a blurred material panel, for argv[1] seconds (default 20).
// Run in the logged-in GUI session: launchctl asuser <uid> ./anim 20 [x y w h] (points, origin bottom-left)
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 20;
        const double wx = argc > 5 ? atof(argv[2]) : 200, wy = argc > 5 ? atof(argv[3]) : 150;
        const double ww = argc > 5 ? atof(argv[4]) : 1400, wh = argc > 5 ? atof(argv[5]) : 900;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(wx, wy, ww, wh)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = @"phx anim";
        NSView *v = w.contentView;
        v.wantsLayer = YES;
        v.layer.backgroundColor = NSColor.systemTealColor.CGColor;
        // PHX_ANIM_BASE: the first layer's animation period in seconds (default 1.0), PHX_ANIM_NOFX=1: no material panel
        const double base = getenv("PHX_ANIM_BASE") ? atof(getenv("PHX_ANIM_BASE")) : 1.0;
        for (int i = 0; i < 12; i++) {
            CALayer *l = [CALayer layer];
            l.frame = CGRectMake(50 + i * 110, 100, 80, 80);
            l.cornerRadius = 16;
            l.backgroundColor = [NSColor colorWithHue:i / 12.0 saturation:0.8 brightness:0.9 alpha:1].CGColor;
            CABasicAnimation *a = [CABasicAnimation animationWithKeyPath:@"position.y"];
            a.fromValue = @100; a.toValue = @800; a.duration = base + i * 0.1; a.autoreverses = YES; a.repeatCount = HUGE_VALF;
            // PHX_ANIM_FPS=n: the animations ask Core Animation for n frames a second (preferredFrameRateRange)
            if (getenv("PHX_ANIM_FPS")) { const float r = atof(getenv("PHX_ANIM_FPS")); a.preferredFrameRateRange = CAFrameRateRangeMake(r, r, r); }
            [l addAnimation:a forKey:@"y"];
            [v.layer addSublayer:l];
        }
        NSVisualEffectView *fx = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(300, 250, 800, 400)];
        fx.material = NSVisualEffectMaterialHUDWindow;
        fx.blendingMode = NSVisualEffectBlendingModeWithinWindow;
        fx.state = NSVisualEffectStateActive;
        if (!getenv("PHX_ANIM_NOFX")) [v addSubview:fx];
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        __block int t = 0;
        // PHX_ANIM_STILL=1: the window stays put (only the layers and the material animate)
        if (!getenv("PHX_ANIM_STILL")) [NSTimer scheduledTimerWithTimeInterval:1 / 60.0 repeats:YES block:^(NSTimer *tm) {
            NSRect f = w.frame;
            f.origin.x = wx + 150 * sin(++t / 30.0);
            [w setFrame:f display:NO];
        }];
        [NSTimer scheduledTimerWithTimeInterval:secs repeats:NO block:^(NSTimer *tm) { [NSApp terminate:nil]; }];
        [NSApp run];
    }
    return 0;
}
