// winlist <pid>: on-screen windows of a process (bounds, layer, alpha) — no screen-recording permission needed.
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    int pid = atoi(argv[1]);
    CFArrayRef a = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    for (CFIndex i = 0; i < CFArrayGetCount(a); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(a, i);
        int p = 0, layer = 0, onscreen = 0; double alpha = 0;
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowOwnerPID), kCFNumberIntType, &p);
        if (p != pid) continue;
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowLayer), kCFNumberIntType, &layer);
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowAlpha), kCFNumberDoubleType, &alpha);
        CFBooleanRef on = CFDictionaryGetValue(d, kCGWindowIsOnscreen);
        onscreen = on && CFBooleanGetValue(on);
        CGRect r; CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(d, kCGWindowBounds), &r);
        printf("layer %d onscreen %d alpha %.2f  %.0fx%.0f at %.0f,%.0f\n", layer, onscreen, alpha, r.size.width, r.size.height, r.origin.x, r.origin.y);
    }
    return 0;
}
