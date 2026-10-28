// nocommit-check: is a status item's IOSurface shown when rewritten in place without
// a Core Animation commit? Shows two squares cycling red, green, blue for 20 s: the
// left one is committed each second, the right one only rewritten in place.
// Build: clang -O1 -fobjc-arc bench/nocommit-check.m -framework AppKit -framework QuartzCore
//        -framework IOSurface -o build/nocommit-check && ./build/nocommit-check
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

static IOSurfaceRef surf, surf_ref[2];
static CALayer *ref_layer;
static int ticks;

static void fill_surface(IOSurfaceRef s, uint32_t c) {
    IOSurfaceLock(s, 0, NULL);
    uint32_t *px = IOSurfaceGetBaseAddress(s);
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s) / 4; i++) px[i] = c;
    IOSurfaceUnlock(s, 0, NULL);
}
static void fill(uint32_t c) { fill_surface(surf, c); }

static void tick(CFRunLoopTimerRef t, void *info) {
    static const uint32_t colors[3] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};  // BGRA: red, green, blue
    static const char *names[3] = {"red", "green", "blue"};
    ticks++;
    fill(colors[ticks % 3]);  // right square: in place, no commit
    IOSurfaceRef r = surf_ref[ticks & 1];  // left square: new contents, committed
    fill_surface(r, colors[ticks % 3]);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    ref_layer.contents = (__bridge id)r;
    [CATransaction commit];
    printf("now %s\n", names[ticks % 3]);
    fflush(stdout);
    if (ticks >= 20) exit(0);
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyAccessory;
        NSStatusItem *item = [NSStatusBar.systemStatusBar statusItemWithLength:40];
        NSStatusItem *ref = [NSStatusBar.systemStatusBar statusItemWithLength:40];  // appears to the left
        ref.button.wantsLayer = YES;
        ref_layer = [CALayer layer];
        ref_layer.frame = CGRectMake(0, 0, 40, 22);
        ref_layer.contentsScale = 2;
        [ref.button.layer addSublayer:ref_layer];
        item.button.wantsLayer = YES;
        CALayer *layer = [CALayer layer];
        layer.frame = CGRectMake(0, 0, 40, 22);
        layer.contentsScale = 2;
        [item.button.layer addSublayer:layer];
        NSDictionary *p = @{(id)kIOSurfaceWidth : @80, (id)kIOSurfaceHeight : @44, (id)kIOSurfaceBytesPerElement : @4,
                            (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')};
        surf = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        surf_ref[0] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        surf_ref[1] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        fill(0xffff0000u);
        fill_surface(surf_ref[0], 0xffff0000u);
        layer.contents = (__bridge id)surf;  // the only commit for the right square
        ref_layer.contents = (__bridge id)surf_ref[0];
        printf("watch the two squares for 20 s: left = committed (reference), right = in place, no commit\nnow red\n");
        fflush(stdout);
        CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1.0, 0, 0, tick, NULL);
        CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
        [NSApp run];
    }
}
