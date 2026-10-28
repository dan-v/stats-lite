// panelbench: instructions per panel frame (full repaint vs incremental), and PNGs.
#include <ImageIO/ImageIO.h>
#include <libproc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include "../src/core/panel.h"

static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }

static void build_old(plist_t *l, int t) {
    char a[48], b[48];
    pl_reset(l);
    snprintf(a, sizeof a, "%d%%", 10 + t % 50);
    pl_header(l, "CPU", a);
    pitem_t *sp = pl_spark(l, PC_BLUE, 0);
    sp->n = HIST_CAP; sp->serial = (uint32_t)t + 1000; sp->vscale = 1;
    for (int i = 0; i < HIST_CAP; i++) sp->v[0][i] = 0.3f + 0.25f * sinf((float)(i + t) * 0.21f) + 0.1f * sinf((float)(i + t) * 1.3f);
    pitem_t *dy = pl_day(l, PC_BLUE);
    dy->n = 120;
    for (int i = 0; i < 120; i++) { dy->v[0][i] = 0.15f + 0.1f * sinf(i * 0.3f); dy->v[1][i] = dy->v[0][i] + 0.2f; }
    pl_row(l, "System", "4%"); pl_row(l, "User", a); pl_row(l, "Idle", "80%");
    pl_row(l, "Performance cores", "12%"); pl_row(l, "Super cores", "3%");
    pitem_t *co = pl_cores(l);
    co->n = 18;
    for (int i = 0; i < 18; i++) { co->v[0][i] = fabsf(sinf((float)(i * 7 + t))); co->tag[i] = i >= 12; }
    pl_row(l, "Load average", "3.12  2.80  2.51");
    pl_gap(l, 4);
    const char *names[] = {"Safari", "WindowServer", "Xcode", "kernel_task", "Google Chrome Helper (Renderer)"};
    // Like the app: processes refresh every 2nd frame, memory every 2nd.
    for (int i = 0; i < 5; i++) { snprintf(b, sizeof b, "%.1f%%", 30.0 / (i + 1) + (t / 2) % 3); pl_small(l, names[i], b); }
    pl_divider(l);
    pl_header(l, "GPU", "7%");
    pitem_t *g = pl_spark(l, PC_PURPLE, 0);
    g->n = 120;
    for (int i = 0; i < 120; i++) g->v[0][i] = 0.05f;
    pl_row(l, "Renderer", "7%"); pl_row(l, "Tiler", "2%");
    pl_divider(l);
    pl_header(l, "Memory", "62%");
    pitem_t *m = pl_spark(l, PC_GREEN, 0);
    m->n = 120;
    for (int i = 0; i < 120; i++) m->v[0][i] = 0.62f;
    snprintf(b, sizeof b, "%.1f GB of 51.5 GB", 30.1 + 0.1 * ((t / 2) % 4)); pl_row(l, "Used", b); pl_row(l, "App", "18.2 GB"); pl_row(l, "Wired", "4.1 GB");
    pl_row(l, "Compressed", "1.2 GB"); pl_row(l, "Cached", "9.3 GB"); pl_row(l, "Swap", "0 B"); pl_row(l, "Pressure", "Normal");
    pl_gap(l, 4);
    for (int i = 0; i < 5; i++) { snprintf(b, sizeof b, "%d MB", 900 - i * 60 + (t / 2) % 3); pl_small(l, names[i], b); }
    pl_divider(l);
    pl_header(l, "Disk", "66%");
    pl_bar(l, 0.66f, PC_TEAL);
    pl_row(l, "Used", "658 GB of 995 GB"); pl_row(l, "Available", "337 GB");
    pl_caption(l, "Right-click the menu bar item for settings");
}

// The compact layout the app builds now: detail lines, process rows with share
// bars, a segmented memory bar with legends.
static void build_new(plist_t *l, int t) {
    char a[64], b[48];
    pl_reset(l);
    snprintf(a, sizeof a, "%d%%", 10 + t % 50);
    pl_header(l, "CPU", a);
    pitem_t *sp = pl_spark(l, PC_BLUE, 0);
    sp->n = HIST_CAP; sp->serial = (uint32_t)t + 1000; sp->vscale = 1;
    for (int i = 0; i < HIST_CAP; i++) sp->v[0][i] = 0.3f + 0.25f * sinf((float)(i + t) * 0.21f) + 0.1f * sinf((float)(i + t) * 1.3f);
    pitem_t *dy = pl_day(l, PC_BLUE);
    dy->n = 120;
    for (int i = 0; i < 120; i++) { dy->v[0][i] = 0.15f + 0.1f * sinf(i * 0.3f); dy->v[1][i] = dy->v[0][i] + 0.2f; }
    static const float slots[3] = {0, 70, 150};
    snprintf(a, sizeof a, "%d%%|4%%|3.12 2.80 2.51", 10 + t % 50);
    pl_kv(l, "user|system|load", a, NULL, slots);
    pitem_t *co = pl_cores(l);
    co->n = 18;
    for (int i = 0; i < 18; i++) { co->v[0][i] = fabsf(sinf((float)(i * 7 + t))); co->tag[i] = i >= 12; }
    static const uint8_t cc[2] = {PC_BLUE, PC_ORANGE};
    snprintf(a, sizeof a, "12%%|3%%");
    pl_kv(l, "Performance|Super", a, cc, NULL);
    pl_gap(l, 4);
    const char *names[] = {"Safari", "WindowServer", "Xcode", "kernel_task", "Google Chrome Helper (Renderer)"};
    for (int i = 0; i < 5; i++) {
        float v = 30.0f / (i + 1) + (t / 2) % 3;
        snprintf(b, sizeof b, "%.1f%%", v);
        pl_proc(l, names[i], b, v / 33.0f, PC_BLUE);
    }
    pl_divider(l);
    pl_header(l, "GPU", "7%");
    pitem_t *g = pl_spark(l, PC_PURPLE, 0);
    g->n = 120;
    for (int i = 0; i < 120; i++) g->v[0][i] = 0.05f;
    pl_kv(l, "renderer|tiler", "7%|2%", NULL, NULL);
    pl_gap(l, 4);
    const char *gnames[] = {"Safari", "WindowServer", "Xcode"};
    for (int i = 0; i < 3; i++) {
        float v = 4.0f / (i + 1) + 0.1f * ((t / 2) % 3);
        snprintf(b, sizeof b, "%.1f%%", v);
        pl_proc(l, gnames[i], b, v / 100, PC_PURPLE);
    }
    pl_divider(l);
    pl_header(l, "Memory", "62%");
    pitem_t *m = pl_spark(l, PC_GREEN, 0);
    m->n = 120;
    for (int i = 0; i < 120; i++) m->v[0][i] = 0.62f;
    pitem_t *st = pl_stack(l);
    st->n = 4;
    const float part[4] = {0.35f + 0.002f * ((t / 2) % 4), 0.08f, 0.02f, 0.18f};
    const uint8_t col[4] = {PC_GREEN, PC_ORANGE, PC_PURPLE, PC_TEAL};
    for (int i = 0; i < 4; i++) { st->v[0][i] = part[i]; st->tag[i] = col[i]; }
    static const uint8_t k1[2] = {PC_GREEN, PC_ORANGE}, k2[2] = {PC_PURPLE, PC_TEAL};
    snprintf(a, sizeof a, "%.1f GB|4.1 GB", 18.2 + 0.1 * ((t / 2) % 4));
    pl_kv(l, "app|wired", a, k1, NULL);
    pl_kv(l, "compressed|cached", "1.2 GB|9.3 GB", k2, NULL);
    snprintf(a, sizeof a, "%.1f GB of 51.5 GB", 30.1 + 0.1 * ((t / 2) % 4));
    pl_kv(l, "used", a, NULL, NULL);
    pl_gap(l, 4);
    for (int i = 0; i < 5; i++) {
        snprintf(b, sizeof b, "%d MB", 900 - i * 60 + (t / 2) % 3);
        pl_proc(l, names[i], b, 0.02f, PC_GREEN);
    }
    pl_divider(l);
    pl_header(l, "Disk", "66%");
    pl_bar(l, 0.66f, PC_TEAL);
    static const float ds[2] = {0, 170};
    pl_kv(l, "free|purgeable", "337 GB of 995 GB|5.4 GB", NULL, ds);
    pl_caption(l, "Right-click the menu bar item for settings");
}

static void build(plist_t *l, int t) { if (getenv("OLD")) build_old(l, t); else build_new(l, t); }
static void png(IOSurfaceRef s, const char *path);
static void png2(IOSurfaceRef st, IOSurfaceRef dy, const char *path) {
    // composite static under dynamic into the static surface copy for the PNG
    IOSurfaceLock(st, 0, NULL);
    IOSurfaceLock(dy, kIOSurfaceLockReadOnly, NULL);
    uint32_t *a = IOSurfaceGetBaseAddress(st), *b = IOSurfaceGetBaseAddress(dy);
    size_t n = IOSurfaceGetAllocSize(st) / 4;
    for (size_t i = 0; i < n; i++) if (b[i]) a[i] = blit_blend(b[i], a[i]);
    IOSurfaceUnlock(dy, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceUnlock(st, 0, NULL);
    png(st, path);
}
static void png(IOSurfaceRef s, const char *path) {
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(IOSurfaceGetBaseAddress(s), IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), 8,
                                           IOSurfaceGetBytesPerRow(s), cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(c);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    // composite on a panel-like background
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    CGContextRef o = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    int dark = strstr(path, "dark") != NULL;
    CGContextSetRGBFillColor(o, dark ? 0.17 : 0.95, dark ? 0.17 : 0.95, dark ? 0.18 : 0.96, 1);
    CGContextFillRect(o, CGRectMake(0, 0, w, h));
    CGContextDrawImage(o, CGRectMake(0, 0, w, h), img);
    CGImageRef out = CGBitmapContextCreateImage(o);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, out, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d); CFRelease(url); CGImageRelease(out); CGImageRelease(img); CGContextRelease(o); CGContextRelease(c); CGColorSpaceRelease(cs);
}

int main(int argc, char **argv) {
    static panel_t p;
    static plist_t l;
    panel_init(&p);
    build(&l, 0);
    float h = panel_measure(&l);
    int painted;
    for (int dark = 1; dark >= 0; dark--) {
        panel_release(&p);
        // full repaints: force by changing scroll alternately
        uint64_t i0 = instr();
        const int N = getenv("N") ? atoi(getenv("N")) : 100;
        for (int t = 0; t < (getenv("INC") ? 0 : N); t++) {
            build(&l, t);
            IOSurfaceRef st = NULL, s = panel_render(&p, &l, h, 2, dark, (float)(t & 1) * 0.0f + (t & 1 ? 0.5f : 0), &painted, &st);
            if (s) CFRelease(s);
            if (st) CFRelease(st);
        }
        uint64_t full = (instr() - i0) / N;
        // incremental: values change every frame like a live panel
        panel_release(&p);
        build(&l, 0);
        IOSurfaceRef st0 = NULL, s0 = panel_render(&p, &l, h, 2, dark, 0, &painted, &st0);
        if (s0) CFRelease(s0);
        IOSurfaceRef stat_keep = st0;
        i0 = instr();
        int rows = 0;
        IOSurfaceRef last = NULL;
        for (int t = 1; t <= N; t++) {
            build(&l, t);
            IOSurfaceRef stx = NULL, s = panel_render(&p, &l, h, 2, dark, 0, &painted, &stx);
            if (stx) { if (stat_keep) CFRelease(stat_keep); stat_keep = stx; }
            rows += painted;
            // Like the compositor: the shown surface stays in use until the next one replaces it.
            if (s && !getenv("NOHOLD")) IOSurfaceIncrementUseCount(s);
            if (last) { if (!getenv("NOHOLD")) IOSurfaceDecrementUseCount(last); CFRelease(last); }
            last = s;
        }
        uint64_t inc = (instr() - i0) / N;
#ifdef PANEL_PROFILE
        extern uint64_t g_pp[4];
        printf("  spark memmove %llu, new columns %llu, corner %llu, copy-back %llu instr/frame\n", g_pp[0] / N, g_pp[1] / N, g_pp[2] / N, g_pp[3] / N);
        memset(g_pp, 0, sizeof g_pp);
#endif
        printf("%s: %d rows, %.0f pt tall | full repaint %llu instr/frame | incremental %llu instr/frame (%.1f rows/frame)\n",
               dark ? "dark " : "light", l.n, h, full, inc, (double)rows / N);
        if (argc > 1 && last) {
            char path[512];
            snprintf(path, sizeof path, "%s/panel-%s.png", argv[1], dark ? "dark" : "light");
            png2(stat_keep, last, path);
        }
        if (last) { if (!getenv("NOHOLD")) IOSurfaceDecrementUseCount(last); CFRelease(last); }
        if (stat_keep) CFRelease(stat_keep);
    }
    return 0;
}
