// floor: the cost floor of a 1 Hz menu bar app on this machine, to see how much of
// Stats Lite's per-tick cost is ours and how much any such app pays.
//   floor 0   status item + aligned 1 Hz CFRunLoopTimer that does nothing
//   floor 1   + swaps the item's layer contents between two IOSurfaces each tick (a CA commit)
//   floor 2   + reads GPU PerformanceStatistics each tick (fastio, as Stats Lite does)
//   floor 3   one surface rewritten in place each tick + [layer setContentsChanged] (a commit)
//   floor 4   one surface rewritten in place each tick, no commit at all
//   floor 5   the 1 Hz timer on a background thread's plain CFRunLoop, doing nothing
//   floor 6   background-thread timer + surface swap committed from that thread
// Build: clang -O2 -fobjc-arc bench/floor.m src/core/fastio.c -framework AppKit -framework QuartzCore
//        -framework IOSurface -framework IOKit -o build/floor
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
@interface CALayer (ContentsChanged)
- (void)setContentsChanged;  // exists on CALayer; not in the public header
@end
#include "../src/core/fastio.h"

static int mode;
static CALayer *layer;
static IOSurfaceRef surf[2];
static io_service_t gpu;
static int flip;

static IOSurfaceRef make(int w, int h, uint32_t v) {
    NSDictionary *p = @{(id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfaceBytesPerElement : @4,
                        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')};
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    IOSurfaceLock(s, 0, NULL);
    uint32_t *px = IOSurfaceGetBaseAddress(s);
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s) / 4; i++) px[i] = v;
    IOSurfaceUnlock(s, 0, NULL);
    return s;
}

static void rewrite(IOSurfaceRef s) {
    IOSurfaceLock(s, 0, NULL);
    uint32_t *px = IOSurfaceGetBaseAddress(s);
    px[flip ^= 1] ^= 0x00ffffffu;  // touch a pixel
    IOSurfaceUnlock(s, 0, NULL);
}

static void tick(CFRunLoopTimerRef t, void *info) {
    if (mode == 3 || mode == 4) {
        rewrite(surf[0]);
        if (mode == 3) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [layer setContentsChanged];
            [CATransaction commit];
        }
        return;
    }
    if (mode == 5) return;
    if (mode == 2) {
        static const fio_key_t k[1] = {FIO_KEY("Device Utilization %")};
        int64_t v[1] = {-1};
        fio_read_numbers(gpu, "PerformanceStatistics", k, 1, v);
    }
    if (mode == 1 || mode == 2 || mode == 6) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contents = (__bridge id)surf[flip ^= 1];
        [CATransaction commit];
        if (mode == 6) [CATransaction flush];
    }
}

static void *timer_thread(void *arg) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, floor(now) + 1, 1.0, 0, 0, tick, NULL);
    CFRunLoopTimerSetTolerance(t, 0.1);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopCommonModes);
    CFRunLoopRun();
    return NULL;
}

int main(int argc, char **argv) {
    mode = argc > 1 ? atoi(argv[1]) : 0;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyAccessory;
        NSStatusItem *item = [NSStatusBar.systemStatusBar statusItemWithLength:40];
        item.button.wantsLayer = YES;
        layer = [CALayer layer];
        layer.frame = CGRectMake(0, 0, 40, 22);
        layer.contentsScale = 2;
        [item.button.layer addSublayer:layer];
        surf[0] = make(80, 44, 0xff336699u);
        surf[1] = make(80, 44, 0xff993366u);
        gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
        layer.contents = (__bridge id)surf[0];
        if (mode >= 5) {
            pthread_t th;
            pthread_create(&th, NULL, timer_thread, NULL);
        } else {
            CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, floor(now) + 1, 1.0, 0, 0, tick, NULL);
            CFRunLoopTimerSetTolerance(t, 0.1);
            CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
        }
        [NSApp run];
    }
    return 0;
}
