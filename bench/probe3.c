// probe3: GPU utilization via IOReport GPU performance-state residency vs
// IOAccelerator PerformanceStatistics (fastio). Instructions per sample.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/fastio.h"

typedef struct IOReportSubscriptionRef *IOReportSubscriptionRef;
CFMutableDictionaryRef IOReportCopyChannelsInGroup(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
IOReportSubscriptionRef IOReportCreateSubscription(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
CFStringRef IOReportChannelGetChannelName(CFDictionaryRef);
CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef);
int32_t IOReportStateGetCount(CFDictionaryRef);
CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef, int32_t);
int64_t IOReportStateGetResidency(CFDictionaryRef, int32_t);

static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }

static double util_from_delta(CFDictionaryRef delta) {
    CFArrayRef ch = CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
    double active = 0, total = 0;
    for (CFIndex i = 0; ch && i < CFArrayGetCount(ch); i++) {
        CFDictionaryRef c = CFArrayGetValueAtIndex(ch, i);
        CFStringRef name = IOReportChannelGetChannelName(c);
        if (!name || CFStringCompare(name, CFSTR("GPUPH"), 0) != kCFCompareEqualTo) continue;
        int32_t n = IOReportStateGetCount(c);
        for (int32_t k = 0; k < n; k++) {
            int64_t r = IOReportStateGetResidency(c, k);
            CFStringRef st = IOReportStateGetNameForIndex(c, k);
            total += r;
            if (st && CFStringCompare(st, CFSTR("OFF"), 0) != kCFCompareEqualTo && CFStringCompare(st, CFSTR("IDLE"), 0) != kCFCompareEqualTo &&
                CFStringCompare(st, CFSTR("DOWN"), 0) != kCFCompareEqualTo) active += r;
        }
    }
    return total > 0 ? active / total : -1;
}

int main(void) {
    CFMutableDictionaryRef chans = IOReportCopyChannelsInGroup(CFSTR("GPU Stats"), CFSTR("GPU Performance States"), 0, 0, 0);
    if (!chans) { puts("no GPU Stats channels"); return 1; }
    CFMutableDictionaryRef sub = NULL;
    IOReportSubscriptionRef s = IOReportCreateSubscription(NULL, chans, &sub, 0, NULL);
    CFDictionaryRef prev = IOReportCreateSamples(s, sub, NULL);
    const int N = 200;
    uint64_t i0 = instr();
    double u = 0;
    for (int i = 0; i < N; i++) {
        usleep(5000);
        CFDictionaryRef cur = IOReportCreateSamples(s, sub, NULL);
        CFDictionaryRef d = IOReportCreateSamplesDelta(prev, cur, NULL);
        u = util_from_delta(d);
        CFRelease(d);
        CFRelease(prev);
        prev = cur;
    }
    uint64_t i1 = instr();
    // usleep costs a little; measure it alone
    uint64_t j0 = instr();
    for (int i = 0; i < N; i++) usleep(5000);
    uint64_t j1 = instr();
    printf("IOReport GPUPH residency: %8.0f instr/sample (last util %.1f%%)\n", ((double)(i1 - i0) - (double)(j1 - j0)) / N, u * 100);

    io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    fio_key_t k[1] = {FIO_KEY("Device Utilization %")};
    int64_t v = -1;
    i0 = instr();
    for (int i = 0; i < N; i++) { usleep(5000); fio_read_numbers(gpu, "PerformanceStatistics", k, 1, &v); }
    i1 = instr();
    printf("PerformanceStatistics (fastio): %8.0f instr/sample (util %lld%%)\n", ((double)(i1 - i0) - (double)(j1 - j0)) / N, v);
    return 0;
}
