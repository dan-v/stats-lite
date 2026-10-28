// probe4: per-process GPU time from AGXDeviceUserClient AppUsage. Instructions per
// sweep: enumeration, CF reads, cached handles with the hand-rolled read.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/fastio.h"

static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }

static io_service_t g_gpu;
static io_object_t g_cl[512];
static int g_n;

static void enumerate(void) {
    for (int i = 0; i < g_n; i++) IOObjectRelease(g_cl[i]);
    g_n = 0;
    io_iterator_t it;
    if (IORegistryEntryGetChildIterator(g_gpu, kIOServicePlane, &it) != KERN_SUCCESS) return;
    io_object_t o;
    while ((o = IOIteratorNext(it)) && g_n < 512) g_cl[g_n++] = o;
    IOObjectRelease(it);
}

static int64_t cf_usage(io_object_t o) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(o, CFSTR("AppUsage"), NULL, 0);
    int64_t s = 0;
    if (v && CFGetTypeID(v) == CFArrayGetTypeID())
        for (CFIndex k = 0; k < CFArrayGetCount(v); k++) {
            CFDictionaryRef d = CFArrayGetValueAtIndex(v, k);
            CFNumberRef n = CFDictionaryGetValue(d, CFSTR("accumulatedGPUTime"));
            int64_t x = 0;
            if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &x);
            s += x;
        }
    if (v) CFRelease(v);
    return s;
}

int main(void) {
    g_gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    const int N = 20;
    fio_key_t key = FIO_KEY("accumulatedGPUTime");

    uint64_t i0 = instr();
    for (int r = 0; r < N; r++) enumerate();
    printf("enumerate %d clients: %llu instr\n", g_n, (instr() - i0) / N);

    i0 = instr();
    for (int r = 0; r < N; r++)
        for (int i = 0; i < g_n; i++) {
            CFTypeRef c = IORegistryEntryCreateCFProperty(g_cl[i], CFSTR("IOUserClientCreator"), NULL, 0);
            if (c) CFRelease(c);
        }
    printf("CF creator x%d: %llu\n", g_n, (instr() - i0) / N);

    i0 = instr();
    for (int r = 0; r < N; r++) for (int i = 0; i < g_n; i++) cf_usage(g_cl[i]);
    printf("CF AppUsage x%d: %llu\n", g_n, (instr() - i0) / N);

    i0 = instr();
    int nonempty = 0;
    for (int r = 0; r < N; r++) {
        nonempty = 0;
        for (int i = 0; i < g_n; i++) { int64_t s; if (fio_sum_key(g_cl[i], "AppUsage", key, &s) > 0) nonempty++; }
    }
    printf("fast AppUsage x%d: %llu (%d non-empty, fast=%d)\n", g_n, (instr() - i0) / N, nonempty, fio_fast_path_active());

    int bad = 0;
    for (int i = 0; i < g_n; i++) {
        int64_t a, b = cf_usage(g_cl[i]);
        if (fio_sum_key(g_cl[i], "AppUsage", key, &a) < 0) a = 0;
        if (a != b && a - b > 100000000) bad++;  // time may advance between the two reads
    }
    printf("mismatches: %d\n", bad);

    i0 = instr();
    for (int r = 0; r < N; r++) { int64_t s; fio_sum_key(g_cl[0], "NoSuchProp", key, &s); }
    printf("bare round trip: %llu\n", (instr() - i0) / N);
    return 0;
}
