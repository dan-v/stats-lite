// Unit tests for the C core. Built with ASan + UBSan (make test).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include "../src/core/fastio.h"
#include "../src/core/history.h"
#include "../src/core/panel.h"
#include <IOSurface/IOSurface.h>
#include <math.h>
#include "../src/core/procs.h"
#include "../src/core/render.h"
#include "../src/core/sampler.h"

static int failures, checks;
// CI runners are VMs: no Apple GPU driver and noisy timing, so those tests are skipped there.
static int on_ci(void) { return getenv("CI") != NULL; }
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- OSSerializeBinary builder ----------------------------------------------------
typedef struct { uint32_t w[512]; size_t n; } stream_t;
static void put(stream_t *s, uint32_t v) { s->w[s->n++] = v; }
static void sym(stream_t *s, const char *k, int last) {
    size_t len = strlen(k) + 1;
    put(s, (last ? 0x80000000u : 0) | 0x08000000u | (uint32_t)len);
    memset(&s->w[s->n], 0, (len + 3) & ~3u);
    memcpy(&s->w[s->n], k, len);
    s->n += (len + 3) / 4;
}
static void num(stream_t *s, uint64_t v, int last) {
    put(s, (last ? 0x80000000u : 0) | 0x04000000u | 64);
    put(s, (uint32_t)v);
    put(s, (uint32_t)(v >> 32));
}
static void str(stream_t *s, const char *v, int last) {
    size_t len = strlen(v);
    put(s, (last ? 0x80000000u : 0) | 0x09000000u | (uint32_t)len);
    memset(&s->w[s->n], 0, (len + 3) & ~3u);
    memcpy(&s->w[s->n], v, len);
    s->n += (len + 3) / 4;
}

static void test_scan(void) {
    stream_t s = {0};
    put(&s, 0xd3);
    put(&s, 0x81000000u | 4);  // dict, 4 entries (count is informational for the scanner)
    sym(&s, "Device Utilization %", 0); num(&s, 42, 0);
    sym(&s, "model", 0); str(&s, "Apple GPU", 0);
    sym(&s, "In use system memory", 0); num(&s, 0x123456789ull, 0);
    sym(&s, "Tiler Utilization %", 0); num(&s, 7, 1);
    fio_key_t keys[3] = {FIO_KEY("Tiler Utilization %"), FIO_KEY("Device Utilization %"), FIO_KEY("In use system memory")};
    int64_t out[3] = {-1, -1, -1};
    CHECK(fio_scan(s.w, s.n, keys, 3, out) == 3, "all keys found");
    CHECK(out[0] == 7 && out[1] == 42 && out[2] == 0x123456789ll, "values %lld %lld %lld", out[0], out[1], out[2]);

    // A key that is a prefix of another must not match.
    fio_key_t pk[1] = {FIO_KEY("Device Utilization")};
    int64_t o1 = -1;
    CHECK(fio_scan(s.w, s.n, pk, 1, &o1) == 0 && o1 == -1, "prefix must not match");

    // A string value equal to a key name must not be treated as a key.
    stream_t t = {0};
    put(&t, 0xd3); put(&t, 0x81000000u | 2);
    sym(&t, "name", 0); str(&t, "Bytes (Read)", 0);
    sym(&t, "x", 0); num(&t, 5, 1);
    fio_key_t bk[1] = {FIO_KEY("Bytes (Read)")};
    int64_t o2 = -1;
    CHECK(fio_scan(t.w, t.n, bk, 1, &o2) == 0 && o2 == -1, "string value is not a key");

    // Bad signature, truncation and unknown types are rejected, never over-read.
    uint32_t bad[2] = {0xd2, 0};
    CHECK(fio_scan(bad, 2, keys, 1, out) == -1, "bad signature");
    for (size_t cut = 1; cut < s.n; cut++) {
        int64_t o[3] = {-1, -1, -1};
        int r = fio_scan(s.w, cut, keys, 3, o);
        CHECK(r >= -1 && r <= 3, "truncated stream at %zu -> %d", cut, r);
    }
    uint32_t unk[3] = {0xd3, 0x7f000000u, 0};
    CHECK(fio_scan(unk, 3, keys, 1, out) == -1, "unknown type");
}

static void ref(stream_t *s, uint32_t idx, int last) { put(s, (last ? 0x80000000u : 0) | 0x0c000000u | idx); }

// AppUsage: an array of dictionaries; repeated key symbols arrive as references to
// their first occurrence (object index, counting every non-reference object).
static void test_scan_sum(void) {
    stream_t s = {0};
    put(&s, 0xd3);
    put(&s, 0x82000000u | 2);                          // 0 array
    put(&s, 0x01000000u | 3);                          // 1 dict
    sym(&s, "API", 0); str(&s, "Metal", 0);            // 2, 3
    sym(&s, "lastSubmittedTime", 0); num(&s, 999, 0);  // 4, 5
    sym(&s, "accumulatedGPUTime", 0); num(&s, 1000, 1); // 6, 7
    put(&s, 0x81000000u | 3);                          // 8 dict, last in the array
    ref(&s, 2, 0); ref(&s, 3, 0);
    ref(&s, 4, 0); num(&s, 5, 0);
    ref(&s, 6, 0); num(&s, 234, 1);
    fio_key_t k = FIO_KEY("accumulatedGPUTime");
    int64_t sum = -1;
    CHECK(fio_scan_sum(s.w, s.n, k, &sum) == 2 && sum == 1234, "sum over referenced keys: %lld", sum);
    fio_key_t other = FIO_KEY("lastSubmittedTime");
    CHECK(fio_scan_sum(s.w, s.n, other, &sum) == 2 && sum == 1004, "other key: %lld", sum);

    stream_t e = {0};
    put(&e, 0xd3); put(&e, 0x82000000u);  // empty array
    CHECK(fio_scan_sum(e.w, e.n, k, &sum) == 0 && sum == 0, "empty");

    // A string value spelling the key is not a key; a shared value goes to CF.
    stream_t v = {0};
    put(&v, 0xd3); put(&v, 0x81000000u | 2);
    sym(&v, "name", 0); str(&v, "accumulatedGPUTime", 0);
    sym(&v, "accumulatedGPUTime", 0); ref(&v, 0, 1);
    CHECK(fio_scan_sum(v.w, v.n, k, &sum) == -1, "referenced value");
    for (size_t cut = 1; cut < s.n; cut++) CHECK(fio_scan_sum(s.w, cut, k, &sum) == -1, "truncated at %zu", cut);
}

// The fast MIG path must agree with the public CF API on real registry entries.
static void test_fastio_live(void) {
    io_service_t gpu = on_ci() ? 0 : IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    if (gpu) {
        fio_key_t k[1] = {FIO_KEY("In use system memory")};
        int64_t fast = -1;
        int r = fio_read_numbers(gpu, "PerformanceStatistics", k, 1, &fast);
        CFDictionaryRef d = IORegistryEntryCreateCFProperty(gpu, CFSTR("PerformanceStatistics"), NULL, 0);
        int64_t slow = -1;
        CFNumberRef n = d ? CFDictionaryGetValue(d, CFSTR("In use system memory")) : NULL;
        if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &slow);
        if (d) CFRelease(d);
        CHECK(r == 1, "gpu read returned %d", r);
        // memory in use moves slightly between reads; same order of magnitude is enough
        CHECK(fast > 0 && slow > 0 && llabs(fast - slow) < slow / 2 + (1 << 24), "gpu mem fast=%lld cf=%lld", fast, slow);
        CHECK(fio_fast_path_active(), "fast path stayed active");
        int64_t miss = -7;
        fio_key_t mk[1] = {FIO_KEY("No Such Key")};
        CHECK(fio_read_numbers(gpu, "PerformanceStatistics", mk, 1, &miss) == 0 && miss == -7, "missing key untouched");
        CHECK(fio_read_numbers(gpu, "NoSuchProperty", mk, 1, &miss) == -1, "missing property -> -1");
        CHECK(fio_fast_path_active(), "missing property must not disable fast path");
        IOObjectRelease(gpu);
    }
    struct statfs st;
    statfs("/", &st);
    io_registry_entry_t e = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, st.f_mntfromname + 5));
    while (e && !IOObjectConformsTo(e, "IOBlockStorageDriver")) {
        io_registry_entry_t p = 0;
        IORegistryEntryGetParentEntry(e, kIOServicePlane, &p);
        IOObjectRelease(e);
        e = p;
    }
    if (e) {
        fio_key_t k[2] = {FIO_KEY("Operations (Read)"), FIO_KEY("Bytes (Read)")};
        int64_t v[2] = {-1, -1};
        CHECK(fio_read_numbers(e, "Statistics", k, 2, v) == 2, "disk stats");
        CFDictionaryRef d = IORegistryEntryCreateCFProperty(e, CFSTR("Statistics"), NULL, 0);
        int64_t ops = -1;
        CFNumberGetValue(CFDictionaryGetValue(d, CFSTR("Operations (Read)")), kCFNumberSInt64Type, &ops);
        CFRelease(d);
        CHECK(v[0] > 0 && v[0] <= ops && ops - v[0] < 100000, "disk ops fast=%lld cf=%lld", v[0], ops);
        IOObjectRelease(e);
    }
}

static void test_sampler(void) {
    sampler_init();
    snapshot_t s;
    memset(&s, 0, sizeof s);
    unsigned all = NEED_CPU | NEED_CPU_CORES | NEED_LOAD | NEED_MEM | NEED_MEM_DETAIL | NEED_GPU | NEED_DISK_SPACE | NEED_DISK_IO | NEED_NET;
    sampler_collect(all, 1, &s);
    usleep(300000);
    sampler_collect(all, 0.3, &s);
    CHECK(s.cpu_total >= 0 && s.cpu_total <= 1.0001f, "cpu %f", s.cpu_total);
    CHECK(s.ncores > 0 && s.nclusters >= 1, "cores %d clusters %d", s.ncores, s.nclusters);
    for (int i = 0; i < s.ncores; i++) CHECK(s.core[i] >= 0 && s.core[i] <= 1.0001f, "core %d = %f", i, s.core[i]);
    CHECK(s.mem_total > (1ull << 30) && s.mem_used > 0 && s.mem_used < s.mem_total, "mem %llu/%llu", s.mem_used, s.mem_total);
    CHECK(s.mem_pressure >= 1, "pressure %d", s.mem_pressure);
    CHECK(s.disk_total > 0 && s.disk_free > 0 && s.disk_free < s.disk_total, "disk %llu/%llu", s.disk_free, s.disk_total);
    CHECK(s.gpu_util >= 0 && s.gpu_util <= 1, "gpu %f", s.gpu_util);
}

static void test_procs(void) {
    proc_row_t c[5], m[5];
    int nc = 0, nm = 0;
    procs_sample(1, c, &nc, m, &nm, 5);
    volatile double x = 0;  // burn some CPU so we show up
    for (int i = 0; i < 30000000; i++) x += i * 0.5;
    procs_sample(0.2, c, &nc, m, &nm, 5);
    CHECK(nc > 0 && nm > 0, "rows %d %d", nc, nm);
    for (int i = 1; i < nc; i++) CHECK(c[i - 1].cpu >= c[i].cpu, "cpu sorted");
    for (int i = 1; i < nm; i++) CHECK(m[i - 1].memory >= m[i].memory, "mem sorted");
    for (int i = 0; i < nc; i++) CHECK(c[i].name[0] != 0 || c[i].pid > 0, "name");
    procs_reset();
}

static void test_render(void) {
    renderer_t r;
    renderer_init(&r);
    CHECK(glyphset_width(&r.value, "100%") > glyphset_width(&r.value, "9%"), "width grows");
    // Monospaced digits: every digit has the same advance.
    for (char d = '1'; d <= '9'; d++) CHECK(r.value.adv[(int)d] == r.value.adv['0'], "digit %c advance", d);
    strip_t st;
    memset(&st, 0, sizeof st);
    st.ncells = 4; st.labels = 1; st.dark = 1; st.scale = 2; st.height = 24;
    for (int i = 0; i < 4; i++) { strcpy(st.cell[i].label, "CPU"); strcpy(st.cell[i].top, "100%"); }
    CGFloat w = renderer_measure(&r, &st);
    CGImageRef img = renderer_draw(&r, &st, w);
    CHECK(img && CGImageGetWidth(img) == (size_t)(w * 2) && CGImageGetHeight(img) == 48, "image size");
    // Something was actually drawn.
    CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
    const uint8_t *p = CFDataGetBytePtr(px);
    size_t lit = 0;
    for (CFIndex i = 3; i < CFDataGetLength(px); i += 4) lit += p[i] > 0;
    CHECK(lit > 500, "lit pixels %zu", lit);
    CFRelease(px);
    CGImageRelease(img);
    // Re-render at a smaller width reuses the context without crashing.
    st.ncells = 1;
    img = renderer_draw(&r, &st, renderer_measure(&r, &st));
    CHECK(img != NULL, "re-render");
    CGImageRelease(img);
}

static void test_history(void) {
    char path[] = "/tmp/lite-history-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    unlink(path);
    CHECK(history_open(path) == 0, "open");
    double t0 = 1800000000.0;  // minute-aligned
    float v[HS_COUNT] = {0};
    // minute 0: cpu 10, 30 -> avg 20 max 30 ; minute 2: cpu 50 ; minute 1 empty
    v[HS_CPU] = 10; history_add(t0 + 1, v, 1u << HS_CPU);
    v[HS_CPU] = 30; history_add(t0 + 2, v, 1u << HS_CPU);
    v[HS_CPU] = 50; v[HS_MEM] = 70; history_add(t0 + 121, v, (1u << HS_CPU) | (1u << HS_MEM));
    static float avg[HISTORY_MINUTES], mx[HISTORY_MINUTES];
    int n = history_series(HS_CPU, t0 + 130, avg, mx);
    int last = HISTORY_MINUTES - 1;
    CHECK(n == 2, "two minutes with data (%d)", n);
    CHECK(avg[last] == 50 && mx[last] == 50, "current minute in progress %f", avg[last]);
    CHECK(isnan(avg[last - 1]), "empty minute is NaN");
    CHECK(avg[last - 2] == 20 && mx[last - 2] == 30, "flushed minute avg %f max %f", avg[last - 2], mx[last - 2]);
    CHECK(history_series(HS_GPU, t0 + 130, avg, mx) == 0, "absent series");
    history_close();
    // Spike attribution attaches to a flushed minute and survives later flushes.
    history_set_top(t0 + 5, "Xcode", 2.5f);
    spike_t sp[3];
    int ns = history_spikes(t0 + 130, 60, 15, sp, 3);
    CHECK(ns == 1 && !strcmp(sp[0].top, "Xcode") && sp[0].top_share == 2.5f && sp[0].cpu_avg == 20, "spike %d", ns);
    CHECK(history_spikes(t0 + 130, 60, 25, sp, 3) == 0, "threshold filters quiet minutes");
    // Reopen: the flushed data persists through the mmap'd file.
    CHECK(history_open(path) == 0, "reopen");
    n = history_series(HS_CPU, t0 + 130, avg, mx);
    CHECK(avg[last - 2] == 20 && avg[last] == 50, "persisted %d %f %f", n, avg[last - 2], avg[last]);
    CHECK(history_spikes(t0 + 130, 60, 15, sp, 3) == 1 && !strcmp(sp[0].top, "Xcode"), "spike persisted");
    // Data older than 24 h is ignored.
    CHECK(history_series(HS_CPU, t0 + 130 + 86400, avg, mx) == 0, "expired");
    // A corrupt header resets the file instead of reading garbage.
    history_close();
    fd = open(path, O_WRONLY);
    write(fd, "XXXX", 4);
    close(fd);
    history_open(path);
    CHECK(history_series(HS_CPU, t0 + 130, avg, mx) == 0, "corrupt header reset");
    history_close();
    unlink(path);
}

static double alpha_sum(CGImageRef img) {
    CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
    const uint8_t *p = CFDataGetBytePtr(px);
    double sum = 0;
    size_t bpr = CGImageGetBytesPerRow(img), w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) sum += p[y * bpr + x * 4 + 3];
    CFRelease(px);
    return sum;
}

// The atlas blitter must produce the same ink as CoreGraphics (within pen snapping).
static void test_blit_matches_cg(void) {
    renderer_t a, b;
    renderer_init(&a);
    renderer_init(&b);
    a.mode = RENDER_IMAGE_EXACT;
    b.mode = RENDER_BLIT_IMAGE;
    strip_t st;
    memset(&st, 0, sizeof st);
    st.ncells = 5; st.labels = 1; st.dark = 1; st.scale = 2; st.height = 24;
    const char *vals[] = {"7%", "100%", "42%", "66%", ""};
    const char *labs[] = {"CPU", "GPU", "RAM", "SSD", ""};
    for (int i = 0; i < 4; i++) { strcpy(st.cell[i].label, labs[i]); strcpy(st.cell[i].top, vals[i]); }
    st.cell[4].kind = CELL_SPEED; strcpy(st.cell[4].top, "12 KB/s"); strcpy(st.cell[4].bottom, "1.4 MB/s");
    for (int dark = 0; dark < 2; dark++) {
        st.dark = (uint8_t)dark;
        CGFloat w = renderer_measure(&a, &st);
        CGImageRef ia = renderer_draw(&a, &st, w), ib = renderer_draw(&b, &st, w);
        double sa = alpha_sum(ia), sb = alpha_sum(ib);
        CHECK(sa > 0 && fabs(sa - sb) / sa < 0.03, "ink differs: cg=%.0f blit=%.0f (dark=%d)", sa, sb, dark);
        CGImageRelease(ia);
        CGImageRelease(ib);
    }
}

// Scrolled sparklines must match a from-scratch render of the same state.
static void spark_list(plist_t *l, int t) {
    pl_reset(l);
    pl_header(l, "CPU", "1%");
    pitem_t *s = pl_spark(l, PC_BLUE, PC_RED);
    s->n = HIST_CAP; s->serial = (uint32_t)t; s->vscale = 1; s->nseries = 2;
    for (int i = 0; i < HIST_CAP; i++) {
        int k = i + t;  // the sample with global index k keeps its value as it scrolls
        s->v[0][i] = 0.5f + 0.4f * sinf(k * 0.37f);
        s->v[1][i] = 0.2f + 0.15f * cosf(k * 0.21f);
    }
    pl_row(l, "System", "2%");
}
// Low chart with one tall spike that scrolls in and out: the shrinking ink top must
// still leave the spike's rows clean (scrolled result == full render).
static void spike_list(plist_t *l, int t) {
    pl_reset(l);
    pitem_t *s = pl_spark(l, PC_BLUE, PC_RED);
    s->n = HIST_CAP; s->serial = (uint32_t)t; s->vscale = 1;
    for (int i = 0; i < HIST_CAP; i++) {
        int k = i + t;
        s->v[0][i] = k == 150 ? 0.95f : 0.04f + 0.01f * (k % 3);
    }
}
static void test_spark_spike_scrolls_out(void) {
    static panel_t a, b;
    static plist_t l;
    panel_init(&a);
    panel_init(&b);
    int painted, bad = 0;
    IOSurfaceRef sa = NULL, st = NULL;
    for (int t = 1; t <= 300; t++) {  // spike enters at t=14, leaves at t=150
        spike_list(&l, t);
        if (sa) CFRelease(sa);
        sa = panel_render(&a, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
        if (st) { CFRelease(st); st = NULL; }
        if (t % 25 == 0 || t == 151 || t == 152) {  // compare against a fresh full render
            panel_release(&b);
            IOSurfaceRef sb = panel_render(&b, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
            if (st) { CFRelease(st); st = NULL; }
            IOSurfaceLock(sa, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceLock(sb, kIOSurfaceLockReadOnly, NULL);
            size_t rows = IOSurfaceGetHeight(sa), rb = IOSurfaceGetWidth(sa) * 4;
            for (size_t y = 0; y < rows; y++)
                bad += memcmp((uint8_t *)IOSurfaceGetBaseAddress(sa) + y * IOSurfaceGetBytesPerRow(sa),
                              (uint8_t *)IOSurfaceGetBaseAddress(sb) + y * IOSurfaceGetBytesPerRow(sb), rb) != 0;
            IOSurfaceUnlock(sb, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceUnlock(sa, kIOSurfaceLockReadOnly, NULL);
            CFRelease(sb);
        }
    }
    CHECK(bad == 0, "spike scrolled out but %d rows differ from a full render", bad);
    CFRelease(sa);
    panel_release(&a);
    panel_release(&b);
}

static void test_spark_scroll(void) {
    static panel_t a, b;
    static plist_t l;
    panel_init(&a);
    panel_init(&b);
    int painted;
    IOSurfaceRef sa = NULL, st = NULL;
    for (int t = 1; t <= 40; t++) {
        spark_list(&l, t);
        if (sa) CFRelease(sa);
        sa = panel_render(&a, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
        if (st) { CFRelease(st); st = NULL; }
    }
    CHECK(a.last_scrolled == 1, "last frame used the scroll path (%d)", a.last_scrolled);
    IOSurfaceRef sb = panel_render(&b, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
    if (st) CFRelease(st);
    IOSurfaceLock(sa, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceLock(sb, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *pa = IOSurfaceGetBaseAddress(sa), *pb = IOSurfaceGetBaseAddress(sb);
    size_t bytes = IOSurfaceGetAllocSize(sa), bad = 0;
    int maxd = 0;
    for (size_t i = 0; i < bytes; i++) {
        int dd = abs((int)pa[i] - (int)pb[i]);
        if (dd > maxd) maxd = dd;
        if (dd > 8) { bad++; if (getenv("SPARK_DEBUG")) printf("  diff at x=%zu y=%zu ch=%zu a=%d b=%d\n", (i % IOSurfaceGetBytesPerRow(sa)) / 4, i / IOSurfaceGetBytesPerRow(sa), i % 4, pa[i], pb[i]); }
    }
    IOSurfaceUnlock(sb, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceUnlock(sa, kIOSurfaceLockReadOnly, NULL);
    CHECK(bad == 0, "scrolled spark differs from full render: %zu bytes off by >8 (max %d)", bad, maxd);
    CFRelease(sa);
    CFRelease(sb);
    panel_release(&a);
    panel_release(&b);
}

// Copy-back: with the shown surface held (as the compositor does), rows changing at
// different rates must still produce exactly what a full render produces.
static void mixed_list(plist_t *l, int t) {
    char a[32], b[32], c[32];
    spark_list(l, t);
    snprintf(a, sizeof a, "%d%%", (t / 2) % 7);
    snprintf(b, sizeof b, "%d MB", 100 + (t / 3) % 5);
    snprintf(c, sizeof c, "%d", t % 11);
    pl_small(l, "every 2nd", a);
    pl_row(l, "every 3rd", b);
    pl_row(l, "every frame", c);
    pitem_t *m = pl_spark(l, PC_GREEN, 0);  // a sparkline that advances every 2nd frame
    m->n = HIST_CAP; m->serial = (uint32_t)(t / 2) + 1; m->vscale = 1;
    for (int i = 0; i < HIST_CAP; i++) m->v[0][i] = 0.3f + 0.2f * sinf((float)(i + t / 2) * 0.5f);
    pl_small(l, "static", "x");
    pl_row(l, "shrinks", (t / 3) % 2 ? "1234567890 GB" : "1");  // old wide ink must be cleared
    pl_header(l, "Head", t % 4 < 2 ? "100%" : "7%");
}
static void test_copy_back(void) {
    static panel_t a, b;
    static plist_t l;
    panel_init(&a);
    panel_init(&b);
    int painted, bad = 0, copied = 0, compared = 0;
    IOSurfaceRef shown = NULL, st = NULL;
    for (int t = 1; t <= 60; t++) {
        mixed_list(&l, t);
        IOSurfaceRef s = panel_render(&a, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
        if (st) { CFRelease(st); st = NULL; }
        if (!s) continue;
        IOSurfaceIncrementUseCount(s);
        if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
        shown = s;
        copied += a.last_copied;
        if (t % 3 == 0 || t > 50) {
            panel_release(&b);
            IOSurfaceRef sb = panel_render(&b, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
            if (st) { CFRelease(st); st = NULL; }
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceLock(sb, kIOSurfaceLockReadOnly, NULL);
            size_t rows = IOSurfaceGetHeight(s), rb = IOSurfaceGetWidth(s) * 4;
            for (size_t y = 0; y < rows; y++) {
                const uint8_t *ra = (uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s);
                const uint8_t *rbp = (uint8_t *)IOSurfaceGetBaseAddress(sb) + y * IOSurfaceGetBytesPerRow(sb);
                int over = 0;  // two-sample scrolls round one edge pixel by at most 1 (pre-existing, invisible)
                for (size_t x = 0; x < rb && !over; x++) over = abs(ra[x] - rbp[x]) > 1;
                if (over) {
                    bad++;
                    if (getenv("CB_DEBUG")) {
                        int x0 = -1, x1 = -1, md = 0;
                        for (size_t x = 0; x < rb; x++) if (ra[x] != rbp[x]) { if (x0 < 0) x0 = (int)x / 4; x1 = (int)x / 4; int dd = abs(ra[x] - rbp[x]); if (dd > md) md = dd; }
                        printf("  t=%d y=%zu x=%d..%d maxdiff=%d\n", t, y, x0, x1, md);
                    }
                }
            }
            IOSurfaceUnlock(sb, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            CFRelease(sb);
            compared++;
        }
    }
    CHECK(copied > 0, "copy-back path was used (%d rows copied)", copied);
    CHECK(bad == 0, "held-surface rendering differs from a full render by >1: %d rows over %d frames", bad, compared);
    if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
    panel_release(&a);
    panel_release(&b);
}

// Partial (changed-cells-only) strip repaints must equal a full repaint.
static void test_strip_partial(void) {
    static renderer_t a, b;
    renderer_init(&a);
    renderer_init(&b);
    a.mode = b.mode = RENDER_BLIT_SURFACE;
    strip_t st;
    memset(&st, 0, sizeof st);
    st.ncells = 5; st.labels = 1; st.dark = 1; st.scale = 2; st.height = 24;
    st.accent[0] = 0.1f; st.accent[1] = 0.5f; st.accent[2] = 1;
    const char *labs[] = {"CPU", "GPU", "RAM", "SSD"};
    for (int i = 0; i < 4; i++) { strcpy(st.cell[i].label, labs[i]); strcpy(st.cell[i].top, "10%"); }
    st.cell[4].kind = CELL_CHART;
    st.cell[4].ncols = 50;
    IOSurfaceRef sa = NULL;
    CGFloat w = renderer_measure(&a, &st);
    for (int t = 0; t < 30; t++) {
        snprintf(st.cell[0].top, sizeof st.cell[0].top, "%d%%", (t * 7) % 100);
        if (t % 5 == 0) snprintf(st.cell[2].top, sizeof st.cell[2].top, "%d%%", 40 + t);
        for (int k = 0; k < 50; k++) st.cell[4].col[k] = (uint8_t)((k + t) % 30);
        if (sa) CFRelease(sa);
        sa = renderer_draw_surface(&a, &st, w);
    }
    IOSurfaceRef sb = renderer_draw_surface(&b, &st, w);
    IOSurfaceLock(sa, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceLock(sb, kIOSurfaceLockReadOnly, NULL);
    size_t rows = IOSurfaceGetHeight(sa), rowbytes = IOSurfaceGetWidth(sa) * 4, bad = 0;
    for (size_t y = 0; y < rows; y++)
        bad += memcmp((uint8_t *)IOSurfaceGetBaseAddress(sa) + y * IOSurfaceGetBytesPerRow(sa),
                      (uint8_t *)IOSurfaceGetBaseAddress(sb) + y * IOSurfaceGetBytesPerRow(sb), rowbytes) != 0;
    IOSurfaceUnlock(sb, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceUnlock(sa, kIOSurfaceLockReadOnly, NULL);
    CHECK(a.values_only > 0, "value-only repaints were used (%u)", a.values_only);
    CHECK(bad == 0, "partial strip repaint differs from full repaint in %zu rows", bad);
    CFRelease(sa);
    CFRelease(sb);
}

// Scrolled strip charts (stacked, plain, mirrored network) must equal full renders,
// including multi-sample steps, filling charts, scale changes and a held surface.
static int chart_v(int k, int which, int h) {
    switch (which) {
    case 0: return (int)lroundf((0.45f + 0.4f * sinf(k * 0.29f)) * h);
    case 1: return (int)lroundf((0.15f + 0.1f * cosf(k * 0.53f)) * h);
    case 2: return k % 17 == 0 ? h : (k % 5);          // spikes and zeros
    default: return (k / 40) % 2 ? (k * 7) % (h / 2 + 1) : 0;  // bursts, mirrored halves
    }
}
static void test_strip_chart_scroll(void) {
    static renderer_t a, b;
    renderer_init(&a);
    renderer_init(&b);
    a.mode = b.mode = RENDER_BLIT_SURFACE;
    strip_t st;
    int bad = 0, compared = 0, ncols = renderer_chart_cols(2), h = renderer_chart_height(2, 0);
    IOSurfaceRef shown = NULL;
    int k = 100;
    for (int t = 0; t < 120; t++) {
        k += (t % 7 == 3) ? 2 : 1;  // sometimes two samples at once
        int boost = (t / 30) % 2;   // scale change: every column changes
        memset(&st, 0, sizeof st);
        st.labels = 1; st.dark = t >= 100; st.scale = 2; st.height = 24;
        st.accent[0] = 0.1f; st.accent[1] = 0.5f; st.accent[2] = 1;
        cell_t *c = &st.cell[st.ncells++];
        strcpy(c->label, "CPU"); snprintf(c->top, sizeof c->top, "%d%%", t % 50);
        for (int m = 0; m < 3; m++) {
            c = &st.cell[st.ncells++];
            c->kind = m == 2 ? CELL_NETCHART : CELL_CHART;
            int n = t < ncols - 12 ? 12 + t : ncols;  // still filling during the first frames
            c->ncols = (uint8_t)n;
            c->stacked = m == 0;
            for (int i = 0; i < n; i++) {
                int kk = k - (n - 1 - i), hh = m == 2 ? h / 2 : h;
                int v = chart_v(kk, m == 0 ? 0 : m == 1 ? 2 : 3, hh) >> boost;
                c->col[i] = (uint8_t)(v > hh ? hh : v);
                if (m == 0) c->col2[i] = (uint8_t)(chart_v(kk, 1, h) >> boost);
                if (m == 2) c->col2[i] = (uint8_t)(chart_v(kk + 13, 3, hh) >> boost);
            }
        }
        IOSurfaceRef s = renderer_draw_surface(&a, &st, renderer_measure(&a, &st));
        if (!s) continue;
        IOSurfaceIncrementUseCount(s);
        if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
        shown = s;
        if (t % 4 == 0 || t < 50 || t > 110) {
            memset(b.surf_valid, 0, sizeof b.surf_valid);  // force a full repaint
            IOSurfaceRef f = renderer_draw_surface(&b, &st, renderer_measure(&b, &st));
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceLock(f, kIOSurfaceLockReadOnly, NULL);
            size_t rows = IOSurfaceGetHeight(f), rb = IOSurfaceGetWidth(f) * 4;
            for (size_t y = 0; y < rows; y++)
                bad += memcmp((uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s),
                              (uint8_t *)IOSurfaceGetBaseAddress(f) + y * IOSurfaceGetBytesPerRow(f), rb) != 0;
            IOSurfaceUnlock(f, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            CFRelease(f);
            compared++;
        }
    }
    CHECK(a.charts_scrolled > 150, "strip charts were scrolled (%u times)", a.charts_scrolled);
    CHECK(bad == 0, "scrolled strip charts differ from full renders: %d rows over %d frames", bad, compared);
    if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
}

// Label-row sparklines with changing text and a held surface must equal full renders.
static void test_strip_sparkline(void) {
    static renderer_t a, b;
    renderer_init(&a);
    renderer_init(&b);
    a.mode = b.mode = RENDER_BLIT_SURFACE;
    strip_t st;
    int bad = 0, compared = 0, cols = renderer_spark_cols(2), h = renderer_spark_height(2);
    IOSurfaceRef shown = NULL;
    unsigned scrolled0 = a.charts_scrolled;
    for (int T = 1; T <= 160; T++) {  // T samples so far
        memset(&st, 0, sizeof st);
        st.labels = 1; st.dark = 1; st.scale = 2; st.height = 24;
        st.accent[0] = 0.1f; st.accent[1] = 0.5f; st.accent[2] = 1;
        for (int m = 0; m < 3; m++) {
            cell_t *c = &st.cell[st.ncells++];
            strcpy(c->label, m == 0 ? "CPU" : m == 1 ? "GPU" : "RAM");
            snprintf(c->top, sizeof c->top, "%d%%", m == 2 ? 60 + (T / 9) % 3 : (T / 5 * (m + 3)) % 40);  // text mostly steady
            c->spark = 1;
            int last = T - 1, qn = last / 2, q0 = qn - cols + 1 < 0 ? 0 : qn - cols + 1;
            for (int q = q0; q <= qn; q++) {
                int v = 0;
                for (int k = 2 * q; k <= 2 * q + 1 && k <= last; k++) {
                    int x = m == 0 ? (int)lroundf((0.4f + 0.35f * sinf(k * 0.23f)) * h) : m == 1 ? (k % 23 == 0 ? h : k % 2) : h / 2 + (k / 20) % 2;
                    if (x > v) v = x;
                }
                c->col[q - q0] = (uint8_t)(v > h ? h : v);
            }
            c->ncols = (uint8_t)(qn - q0 + 1);
        }
        IOSurfaceRef s = renderer_draw_surface(&a, &st, renderer_measure(&a, &st));
        if (!s) continue;
        IOSurfaceIncrementUseCount(s);
        if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
        shown = s;
        if (T % 3 == 0 || T > 150) {
            memset(b.surf_valid, 0, sizeof b.surf_valid);
            IOSurfaceRef f = renderer_draw_surface(&b, &st, renderer_measure(&b, &st));
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceLock(f, kIOSurfaceLockReadOnly, NULL);
            size_t rows = IOSurfaceGetHeight(f), rb = IOSurfaceGetWidth(f) * 4;
            for (size_t y = 0; y < rows; y++)
                bad += memcmp((uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s),
                              (uint8_t *)IOSurfaceGetBaseAddress(f) + y * IOSurfaceGetBytesPerRow(f), rb) != 0;
            IOSurfaceUnlock(f, kIOSurfaceLockReadOnly, NULL);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            CFRelease(f);
            compared++;
        }
    }
    CHECK(a.charts_scrolled - scrolled0 > 50, "sparklines were scrolled (%u times)", a.charts_scrolled - scrolled0);
    CHECK(bad == 0, "scrolled sparklines differ from full renders: %d rows over %d frames", bad, compared);
    if (shown) { IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
}

// Long multi-byte names must be truncated at character boundaries (valid UTF-8).
static void test_utf8_truncation(void) {
    static plist_t l;
    const char *names[] = {"Système de préférences très très très très long nom", "日本語のとても長いアプリケーション名前テスト", "🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂🙂"};
    for (int k = 0; k < 3; k++) {
        for (int pad = 0; pad < 4; pad++) {  // shift the cut point across byte positions
            char src[160];
            snprintf(src, sizeof src, "%.*s%s", pad, "abc", names[k]);
            pl_reset(&l);
            pl_small(&l, src, "1%");
            CFStringRef cs = CFStringCreateWithCString(NULL, l.item[0].a, kCFStringEncodingUTF8);
            CHECK(cs != NULL, "invalid UTF-8 after truncation (name %d, pad %d)", k, pad);
            if (cs) CFRelease(cs);
        }
    }
}

// A process that has been idle (cold tier, rarely read) and starts burning CPU must
// show up quickly through the unexplained-CPU trigger.
static void test_procs_wakeup(void) {
    if (on_ci()) return;
    pid_t child = fork();
    if (child == 0) {
        sleep(7);
        volatile double x = 0;
        for (;;) x += 1.0;
    }
    procs_reset();
    procs_set_full_every(0);
    sampler_init();
    snapshot_t s;
    memset(&s, 0, sizeof s);
    proc_row_t c[5], m[5];
    int nc, nm, found_at = -1;
    double t0 = CFAbsoluteTimeGetCurrent();
    for (int i = 0; i < 40 && found_at < 0; i++) {  // 0.5 s refreshes, up to 20 s
        usleep(500000);
        sampler_collect(NEED_CPU, 0.5, &s);
        procs_set_busy_cores(s.cpu_total * (float)sysconf(_SC_NPROCESSORS_ONLN));
        procs_sample(0.5, c, &nc, m, &nm, 5);
        double t = CFAbsoluteTimeGetCurrent() - t0;
        for (int k = 0; k < nc; k++)
            if (c[k].pid == child && c[k].cpu > 0.5f && t > 7) { found_at = (int)((t - 7) * 1000); break; }
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    CHECK(found_at >= 0 && found_at < 3000, "woken process listed after %d ms (cold period 12 alone would be up to 6 s)", found_at);
    printf("  wake-up detected after %d ms\n", found_at);
    procs_reset();
}

static void test_gpuprocs(void) {
    if (on_ci()) return;
    proc_row_t r[5];
    int n = -1, ok = 0;
    for (int i = 0; i < 3; i++) { ok = gpuprocs_sample(-1, r, &n, 5); usleep(100000); }
    CHECK(ok && n >= 0 && n <= 5, "gpuprocs ok %d n %d", ok, n);
    for (int i = 0; i < n; i++) {
        CHECK(r[i].pid > 0 && r[i].name[0] && r[i].cpu > 0 && r[i].cpu < 2, "row %d: pid %d %s %f", i, r[i].pid, r[i].name, r[i].cpu);
        CHECK(i == 0 || r[i - 1].cpu >= r[i].cpu, "sorted");
    }
    gpuprocs_reset();
    gpuprocs_sample(-1, r, &n, 5);
    CHECK(gpuprocs_last_reads() > 0, "reopen reads every client once");
}

int main(void) {
    test_spark_spike_scrolls_out();
    test_procs_wakeup();
    test_utf8_truncation();
    test_strip_partial();
    test_strip_chart_scroll();
    test_strip_sparkline();
    test_spark_scroll();
    test_copy_back();
    test_blit_matches_cg();
    test_history();
    test_scan();
    test_scan_sum();
    test_gpuprocs();
    test_fastio_live();
    test_sampler();
    test_procs();
    test_render();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
