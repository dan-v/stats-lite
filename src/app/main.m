// Stats Lite: a resource-minimal menu bar system monitor.
// One thread and one timer; all modules render into one surface on one status item.
// Only visible metrics are sampled; battery and the primary interface are event-driven.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/ps/IOPowerSources.h>
#include <libproc.h>
#include <sys/resource.h>

#include "../core/history.h"

#ifndef LITE_DEV
#define LITE_DEV 0  // make DEV=1: profiler (LITE_PROFILE), surface dumps (LITE_DUMP), event logs (LITE_DEBUG)
#endif
#if LITE_DEV
#define LITE_LOG(...) do { if (getenv("LITE_DEBUG")) NSLog(__VA_ARGS__); } while (0)
#else
#define LITE_LOG(...) do { } while (0)
#endif
#include "../core/panel.h"
#include "../core/procs.h"
#include "../core/render.h"
#include "../core/sampler.h"

// ---- Modules -------------------------------------------------------------------
typedef enum { M_CPU, M_GPU, M_MEM, M_DISK, M_NET, M_SENSORS, M_BATTERY, M_BLUETOOTH, M_CLOCK, M_COUNT } module_t;
static NSString *const kModuleKey[M_COUNT] = {@"cpu", @"gpu", @"mem", @"disk", @"net", @"sensors", @"battery", @"bluetooth", @"clock"};
static NSString *const kModuleTitle[M_COUNT] = {@"CPU", @"GPU", @"Memory", @"Disk", @"Network", @"Sensors", @"Battery", @"Bluetooth", @"Clock"};
// App-level "needs" beyond the sampler's: a tick for the clock, a periodic accessory refresh.
enum { NEED_CLOCK = 1 << 20, NEED_BT = 1 << 21 };
static const unsigned kModuleNeed[M_COUNT] = {NEED_CPU, NEED_GPU, NEED_MEM, NEED_DISK_SPACE, NEED_NET, NEED_SENSORS, 0, NEED_BT, NEED_CLOCK};
static const unsigned kPanelNeed[M_COUNT] = {NEED_CPU | NEED_CPU_CORES | NEED_LOAD, NEED_GPU, NEED_MEM | NEED_MEM_DETAIL,
                                             NEED_DISK_SPACE | NEED_DISK_IO, NEED_NET, NEED_SENSORS, 0, NEED_BT, NEED_CLOCK};

#define SPIKE_THRESHOLD 0.15  // minute CPU average that triggers process attribution

enum { STYLE_TEXT = 1, STYLE_CHART = 2, STYLE_BOTH = 3, STYLE_SPARK = 4 };  // SPARK: text + label-row sparkline
static NSString *const kStyleName[5] = {nil, @"text", @"chart", @"both", @"spark"};

// ---- History rings (for panel charts; pushing is a store + increment) -----------
#define HIST 137  // = HIST_CAP: one sample per 2 pt of sparkline (scrolls by whole pixels)
typedef struct { float v[HIST]; int head, count; uint32_t total; } ring_t;
static void ring_push(ring_t *r, float v) {
    r->v[r->head] = v;
    r->head = (r->head + 1) % HIST;
    if (r->count < HIST) r->count++;
    r->total++;
}
static float ring_at(const ring_t *r, int i) { return r->v[(r->head - r->count + i + HIST) % HIST]; }  // i: 0 oldest

typedef enum { H_CPU, H_CPU_SYS, H_GPU, H_MEM, H_DISK_R, H_DISK_W, H_NET_IN, H_NET_OUT, H_TEMP, H_COUNT } hist_t;

// ---- Global state (plain C: the hot path touches no Objective-C objects) --------
static struct {
    snapshot_t snap;
    renderer_t renderer;
    strip_t last;
    CGFloat last_width, last_height;
    int has_last;
    unsigned modules;  // bit per module_t
    int labels;
    double last_tick;
    ring_t hist[H_COUNT];
    int screens_asleep, locked, inactive, occluded, system_asleep;
    CFRunLoopTimerRef timer;
    int panel_open;
    proc_row_t top_cpu[6], top_mem[6], top_gpu[6];
    int ntop_cpu, ntop_mem, ntop_gpu, procs_valid, gpu_procs_valid;
    // Benchmark knobs (hidden defaults, read at launch) for cost attribution.
    int bench_skip;     // 1: sample only once, 2: never render, 4: render but never upload
    int timer_kind;     // 0: CFRunLoopTimer aligned to seconds, 1: unaligned, 2: dispatch timer on main queue
    double tolerance;   // fraction of the interval
    dispatch_source_t dtimer;
    uint8_t style[M_COUNT];  // STYLE_* per module
    float env_accent[3];
    uint32_t step;      // base intervals elapsed (per-metric cadence)
    int thermal;        // NSProcessInfoThermalState, updated by notification (never polled)
    uint32_t attr_minute;  // spike attribution: minute being accumulated
    double attr_sum;
    int attr_n, attr_baseline;
    int mono;           // monochrome menu bar charts
    int force_all;      // sample every visible metric on the next tick
    // Display environment (appearance/scale), refreshed on change notifications.
    int env_dark;
    float env_scale, env_height;
} S;
static CALayer *gContent;  // our own sublayer holding the strip's IOSurface

static NSStatusItem *gItem;
static NSView *gStrip;
static NSPanel *gPanel;
static id gGlobalMonitor;
static CFRunLoopSourceRef gBatterySource;
static SCDynamicStoreRef gNetStore;
static CFRunLoopSourceRef gNetSource;

static void tick(void);
static void schedule_timer(int fire_now);
static void update_strip(int force);
static void apply_modules(void);
static void refresh_env(void);
static void request_redraw(void);
static void request_tick(void);
static void panel_refresh(void);

// Per-module refresh in seconds (menu setting "every.<module>"). The GPU driver already
// averages over about a second, so 2 s loses nothing; battery is event-driven.
static const uint8_t kEveryDefault[M_COUNT] = {1, 2, 1, 60, 1, 3, 1, 60, 60};
// Faster caps while the panel shows the slow metrics live.
static const uint8_t kPanelCap[M_COUNT] = {255, 255, 255, 10, 255, 2, 255, 10, 1};
static uint8_t gEvery[M_COUNT];  // current setting per module, seconds
static uint8_t gEveryDiskIO = 1;  // disk read/write rates (panel)

static int module_period(int m) {
    int p = gEvery[m] ? gEvery[m] : kEveryDefault[m];
    if (S.panel_open && kPanelCap[m] < p) p = kPanelCap[m];
    return p;
}
static int gcd_int(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

// The timer period is the gcd of the visible metrics' periods, so every metric lands on a tick.
static int base_period(void) {
    int g = 0;
    for (int m = 0; m < M_COUNT; m++)
        if ((S.modules & (1u << m)) && kModuleNeed[m]) g = gcd_int(module_period(m), g);
    if (S.panel_open && (S.modules & (1u << M_DISK))) g = gcd_int(gEveryDiskIO, g);
    if (S.panel_open && (S.modules & ((1u << M_CPU) | (1u << M_MEM)))) g = gcd_int(2, g);  // top processes every 2 s
    return g ? g : 60;
}

static unsigned module_need_at(uint32_t step, int all) {
    unsigned need = 0;
    for (int m = 0; m < M_COUNT; m++) {
        if (!(S.modules & (1u << m))) continue;
        if (all || step % (uint32_t)module_period(m) == 0) need |= S.panel_open ? kPanelNeed[m] & ~NEED_DISK_IO : kModuleNeed[m];
        // Disk I/O rates have their own cadence; only free space follows the disk module's.
        if (m == M_DISK && S.panel_open && (all || step % gEveryDiskIO == 0)) need |= NEED_DISK_IO;
    }
    return need;
}
static unsigned module_need(void) { return module_need_at(0, 1); }

#if LITE_DEV
// Optional per-phase instruction profile (LITE_PROFILE=1): reads this process's
// retired-instruction counter between phases and prints averages every 30 ticks.
static int gProf = -1;
static uint64_t gProfAcc[8], gProfLast;
static int gProfTicks;
static const char *kProfName[8] = {"sample", "rings+history", "menu bar", "procs", "panel list", "panel position", "panel render", ""};
static uint64_t self_instr(void) {
    struct rusage_info_v6 ru;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru);
    return ru.ri_instructions;
}
static inline void prof_mark(int phase) {
    if (gProf <= 0) return;
    uint64_t now = self_instr();
    if (phase >= 0) gProfAcc[phase] += now - gProfLast;
    gProfLast = self_instr();  // exclude the cost of the counter read itself
}
static void prof_report(void) {
    if (gProf <= 0 || ++gProfTicks < 30) return;
    fprintf(stderr, "profile (k instr/tick):");
    for (int i = 0; i < 7; i++) fprintf(stderr, " %s %.0f |", kProfName[i], gProfAcc[i] / 30.0 / 1e3);
    fprintf(stderr, "\n");
    memset(gProfAcc, 0, sizeof gProfAcc);
    gProfTicks = 0;
}

#else
static inline void prof_mark(int phase) { (void)phase; }
static inline void prof_report(void) {}
#endif

// ---- Formatting (no allocation) -----------------------------------------------------
static int clock_12h(void) {  // from the user's locale, resolved once
    static int v = -1;
    if (v < 0) v = [[NSDateFormatter dateFormatFromTemplate:@"j" options:0 locale:NSLocale.currentLocale] containsString:@"a"];
    return v;
}
static void fmt_pct(char *out, float frac) {
    int v = (int)lroundf(frac * 100.0f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    int n = 0;
    if (v >= 100) out[n++] = '1';
    if (v >= 10) out[n++] = (char)('0' + (v / 10) % 10);
    out[n++] = (char)('0' + v % 10);
    out[n++] = '%';
    out[n] = 0;
}

static void fmt_speed(char *out, size_t cap, double bps) {
    if (bps < 1e6) snprintf(out, cap, "%d KB/s", (int)lround(bps / 1e3));
    else if (bps < 1e9) snprintf(out, cap, "%.1f MB/s", bps / 1e6);
    else snprintf(out, cap, "%.2f GB/s", bps / 1e9);
}

static void fmt_bytes(char *out, size_t cap, double b) {
    if (b < 1e3) snprintf(out, cap, "%.0f B", b);
    else if (b < 1e6) snprintf(out, cap, "%.0f KB", b / 1e3);
    else if (b < 1e9) snprintf(out, cap, "%.1f MB", b / 1e6);
    else if (b < 1e12) snprintf(out, cap, "%.1f GB", b / 1e9);
    else snprintf(out, cap, "%.2f TB", b / 1e12);
}

static float mem_frac(void) { return S.snap.mem_total ? (float)S.snap.mem_used / (float)S.snap.mem_total : 0; }
static float disk_used_frac(void) {
    return S.snap.disk_total ? (float)(S.snap.disk_total - S.snap.disk_free) / (float)S.snap.disk_total : 0;
}

// ---- Strip view: layer contents only, never drawRect ------------------------------
@interface StripView : NSView
@end
@implementation StripView
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        self.layer.contentsGravity = kCAGravityBottomLeft;
    }
    return self;
}
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {}
- (NSView *)hitTest:(NSPoint)p { return nil; }  // clicks go to the status bar button
- (void)viewDidChangeEffectiveAppearance { refresh_env(); request_redraw(); }
- (void)viewDidChangeBackingProperties { refresh_env(); request_redraw(); }
@end

static int strip_is_dark(void) {
    NSAppearanceName n = [gStrip.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [n isEqualToString:NSAppearanceNameDarkAqua];
}

static void build_text_cell(strip_t *st, int m);

// Quantize the newest samples of a ring into chart column heights (device px).
// max <= 0: auto-scale to the window maximum (never below `floor_max`).
static void chart_cols(cell_t *c, const ring_t *r, const ring_t *r2, float scale, float max, float floor_max) {
    int cols = renderer_chart_cols(scale), half = r2 != NULL;
    int h = renderer_chart_height(scale, half);
    int n = r->count < cols ? r->count : cols;
    if (max <= 0) {
        float peak = floor_max;
        for (int i = r->count - n; i < r->count; i++) {
            peak = fmaxf(peak, ring_at(r, i));
            if (r2) peak = fmaxf(peak, ring_at(r2, i));
        }
        // Quantize the scale to 1-2-5 steps: the chart only rescales when the peak
        // crosses a step, not on every new maximum.
        float decade = powf(10, floorf(log10f(peak)));
        max = peak <= decade ? decade : peak <= 2 * decade ? 2 * decade : peak <= 5 * decade ? 5 * decade : 10 * decade;
    }
    c->ncols = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        int k = r->count - n + i;
        float v = ring_at(r, k) / max;
        c->col[i] = (uint8_t)lroundf(fminf(fmaxf(v, 0), 1) * h);
        if (r2) c->col2[i] = (uint8_t)lroundf(fminf(fmaxf(ring_at(r2, k) / max, 0), 1) * h);
    }
}

// Label-row sparkline: two samples per column, keeping the higher so spikes survive.
// Pairs are anchored to the absolute sample count, so older columns only shift.
static void spark_cols(cell_t *c, const ring_t *r, float scale, float max) {
    int cols = renderer_spark_cols(scale), h = renderer_spark_height(scale);
    c->spark = 1;
    c->ncols = 0;
    if (!r->count) return;
    uint32_t last = r->total - 1, first = r->total - (uint32_t)r->count;
    uint32_t qn = last / 2, q0 = first / 2;
    if (qn - q0 + 1 > (uint32_t)cols) q0 = qn - (uint32_t)cols + 1;
    int n = (int)(qn - q0 + 1);
    for (int j = 0; j < n; j++) {
        uint32_t a0 = 2 * (q0 + (uint32_t)j);
        float v = 0;
        for (uint32_t a = a0; a <= a0 + 1 && a <= last; a++)
            if (a >= first) v = fmaxf(v, ring_at(r, (int)(a - first)));
        c->col[j] = (uint8_t)lroundf(fminf(fmaxf(v / max, 0), 1) * (float)h);
    }
    c->ncols = (uint8_t)n;
}

static void build_strip_env(strip_t *st, int dark, float scale, float height) {
    memset(st, 0, sizeof(*st));
    st->labels = (uint8_t)S.labels;
    st->dark = (uint8_t)dark;
    st->scale = scale;
    st->height = height;
    for (int k = 0; k < 3; k++) st->accent[k] = S.env_accent[k];
    st->mono = (uint8_t)S.mono;
    for (int m = 0; m < M_COUNT; m++) {
        if (!(S.modules & (1u << m))) continue;
        int style = S.style[m] ? S.style[m] : STYLE_TEXT;
        const ring_t *r = m == M_CPU ? &S.hist[H_CPU] : m == M_GPU ? &S.hist[H_GPU] : m == M_MEM ? &S.hist[H_MEM] :
                          m == M_NET ? &S.hist[H_NET_OUT] : m == M_SENSORS ? &S.hist[H_TEMP] : NULL;
        int chart = (style & STYLE_CHART) && r && st->ncells < STRIP_MAX_CELLS;
        int text = (style & STYLE_TEXT) || style == STYLE_SPARK || !r;
        if (text) build_text_cell(st, m);
        if (style == STYLE_SPARK && r && m != M_NET && st->ncells > 0 && st->cell[st->ncells - 1].kind == CELL_MINI)
            spark_cols(&st->cell[st->ncells - 1], r, scale, m == M_SENSORS ? 110 : 1);
        if (chart && st->ncells < STRIP_MAX_CELLS) {
            cell_t *c = &st->cell[st->ncells++];
            c->kind = m == M_NET ? CELL_NETCHART : CELL_CHART;
            if (m == M_NET) chart_cols(c, &S.hist[H_NET_OUT], &S.hist[H_NET_IN], scale, 0, 20e3f);
            else if (m == M_CPU) {
                chart_cols(c, &S.hist[H_CPU], NULL, scale, 1, 0);
                cell_t sys = *c;
                chart_cols(&sys, &S.hist[H_CPU_SYS], NULL, scale, 1, 0);
                memcpy(c->col2, sys.col, sizeof c->col2);
                c->stacked = 1;
            } else chart_cols(c, r, NULL, scale, m == M_SENSORS ? 110 : 1, 0);
        }
    }
}

static void build_text_cell(strip_t *st, int m) {
    const snapshot_t *s = &S.snap;
    if (st->ncells >= STRIP_MAX_CELLS) return;
    {
        cell_t *c = &st->cell[st->ncells++];
        c->kind = CELL_MINI;
        switch (m) {
        case M_CPU:
            // "CPU!" and the warning color while the system is throttling (thermal state serious+)
            strcpy(c->label, S.thermal >= NSProcessInfoThermalStateSerious ? "CPU!" : "CPU");
            c->warn = S.thermal >= NSProcessInfoThermalStateSerious;
            fmt_pct(c->top, s->cpu_total);
            break;
        case M_GPU:
            strcpy(c->label, "GPU");
            if (s->gpu_ok) fmt_pct(c->top, s->gpu_util); else strcpy(c->top, "-");
            break;
        case M_MEM: strcpy(c->label, "RAM"); fmt_pct(c->top, mem_frac()); c->warn = s->mem_pressure >= 4; break;
        case M_DISK: strcpy(c->label, "SSD"); fmt_pct(c->top, disk_used_frac()); break;
        case M_NET:
            c->kind = CELL_SPEED;
            fmt_speed(c->top, sizeof(c->top), s->net_out_bps);
            fmt_speed(c->bottom, sizeof(c->bottom), s->net_in_bps);
            break;
        case M_SENSORS:
            strcpy(c->label, "TMP");
            if (s->temp_cpu > 0) snprintf(c->top, sizeof(c->top), "%d\x01", (int)lroundf(s->temp_cpu));
            else strcpy(c->top, "-");
            break;
        case M_CLOCK: {
            time_t t = time(NULL);
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(c->label, sizeof c->label, "%a", &tmv);
            for (char *q = c->label; *q; q++) *q = (char)toupper((unsigned char)*q);
            if (clock_12h()) {
                strftime(c->top, sizeof c->top, "%I:%M", &tmv);
                if (c->top[0] == '0') memmove(c->top, c->top + 1, strlen(c->top));
                strlcat(c->label, tmv.tm_hour < 12 ? " AM" : " PM", sizeof c->label);
            } else {
                strftime(c->top, sizeof c->top, "%H:%M", &tmv);
            }
            c->wide = 38;  // "88:88" at 12 pt with margins; stable width
            break;
        }
        case M_BLUETOOTH: {
            if (!s->nbt) { st->ncells--; break; }
            int lo = 0;  // show the device with the lowest battery
            for (int i = 1; i < s->nbt; i++) if (s->bt[i].level < s->bt[lo].level) lo = i;
            strcpy(c->label, "BT");
            fmt_pct(c->top, s->bt[lo].level);
            break;
        }
        case M_BATTERY:
            if (!s->batt_present) { st->ncells--; break; }
            c->kind = CELL_BATTERY;
            c->level = (uint8_t)lroundf(s->batt_level * 100);
            c->charging = (uint8_t)s->batt_charging;
            break;
        }
    }
}

// Main thread only: snapshot appearance/scale for the rendering thread.
static void refresh_env(void) {
    CGFloat scale = gStrip.window ? gStrip.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    S.env_dark = strip_is_dark();
    S.env_scale = (float)(scale > 0 ? scale : 2);
    S.env_height = (float)NSStatusBar.systemStatusBar.thickness;
    NSColor *a = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    S.env_accent[0] = (float)a.redComponent;
    S.env_accent[1] = (float)a.greenComponent;
    S.env_accent[2] = (float)a.blueComponent;
}

static void build_strip(strip_t *st) { build_strip_env(st, S.env_dark, S.env_scale, S.env_height); }

static id gTipOwner;  // the app delegate (answers view:stringForToolTip:...)
static void set_width_main(CGFloat w, CGFloat h) {
    gItem.length = w;
    gStrip.frame = NSMakeRect(0, 0, w, h);
    if (gTipOwner) {  // the tooltip rect follows the item's width
        [gItem.button removeAllToolTips];
        [gItem.button addToolTipRect:NSMakeRect(0, 0, w, h) owner:gTipOwner userData:NULL];
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    gContent.frame = CGRectMake(0, 0, w, h);
    [CATransaction commit];
}

static void update_strip(int force) {
    if (!gStrip) return;
    strip_t st;
    build_strip(&st);
    if (!force && S.has_last && memcmp(&st, &S.last, sizeof(st)) == 0) return;  // nothing visible changed
    CGFloat w = renderer_measure(&S.renderer, &st);
    if (w != S.last_width || st.height != S.last_height) {
        S.last_width = w;
        S.last_height = st.height;
        CGFloat h = st.height;
        set_width_main(w, h);
    }
    if (S.bench_skip & 2) { S.last = st; S.has_last = 1; return; }
    int surface = S.renderer.mode == RENDER_SURFACE || S.renderer.mode == RENDER_BLIT_SURFACE;
    CFTypeRef contents = surface ? (CFTypeRef)renderer_draw_surface(&S.renderer, &st, w) : (CFTypeRef)renderer_draw(&S.renderer, &st, w);
    if (contents && !(S.bench_skip & 4)) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        gContent.contentsScale = st.scale;
        gContent.contents = (__bridge id)contents;
        [CATransaction commit];
    }
    if (!contents) { S.has_last = 0; return; }  // nothing presented: retry next tick
    CFRelease(contents);
    S.last = st;
    S.has_last = 1;
}

// ---- Panel ------------------------------------------------------------------------
// A display list rendered by src/core/panel.c into two IOSurfaces (static labels and
// backgrounds, dynamic values) over the native popover blur.
static panel_t gPR;
static plist_t gPL;
static float gPanelScroll;

static void panel_colors(NSView *v) {
    NSAppearance *ap = v.effectiveAppearance;
    int dark = [[ap bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]] isEqualToString:NSAppearanceNameDarkAqua];
    [ap performAsCurrentDrawingAppearance:^{
        NSColorSpace *srgb = NSColorSpace.sRGBColorSpace;
        NSColor *pri = [NSColor.labelColor colorUsingColorSpace:srgb], *sec = [NSColor.secondaryLabelColor colorUsingColorSpace:srgb];
        colorlut_init(&gPR.lut_primary[dark], (float)pri.redComponent, (float)pri.greenComponent, (float)pri.blueComponent, (float)pri.alphaComponent);
        colorlut_init(&gPR.lut_secondary[dark], (float)sec.redComponent, (float)sec.greenComponent, (float)sec.blueComponent, (float)sec.alphaComponent);
        NSColor *pal[PC_COUNT] = {NSColor.systemBlueColor, NSColor.systemPurpleColor, NSColor.systemGreenColor, NSColor.systemTealColor,
                                  NSColor.systemPinkColor, NSColor.systemOrangeColor, NSColor.systemRedColor, NSColor.controlAccentColor};
        for (int k = 0; k < PC_COUNT; k++) {
            NSColor *c = [pal[k] colorUsingColorSpace:srgb];
            gPR.palette[dark][k] = premul_rgba((float)c.redComponent, (float)c.greenComponent, (float)c.blueComponent, 1);
        }
    }];
}

// Display names (localized app name, else the process name), cached in C by
// (pid, start time): no Objective-C work per row per tick.
typedef struct { pid_t pid; uint64_t start; char name[48]; } appname_t;
static appname_t gNames[32];
static unsigned gNamesNext;
static const char *app_name(const proc_row_t *r) {
    for (int i = 0; i < 32; i++)
        if (gNames[i].pid == r->pid && gNames[i].start == r->start) return gNames[i].name;
    appname_t *e = &gNames[gNamesNext++ % 32];
    e->pid = r->pid;
    e->start = r->start;
    NSString *n = [NSRunningApplication runningApplicationWithProcessIdentifier:r->pid].localizedName;
    if (!n.length || ![n getCString:e->name maxLength:sizeof e->name encoding:NSUTF8StringEncoding])
        strlcpy(e->name, r->name[0] ? r->name : "?", sizeof e->name);
    return e->name;
}

static void ring_to(pitem_t *it, int series, const ring_t *r, float scale) {
    int n = r->count < HIST_CAP ? r->count : HIST_CAP;
    for (int i = 0; i < n; i++) it->v[series][i] = scale > 0 ? ring_at(r, r->count - n + i) / scale : 0;
    it->n = (uint8_t)n;
    it->serial = r->total;  // lets the renderer scroll instead of redrawing
    it->vscale = scale;
}

// Auto-scale rounded up to a 1-2-5 step, so the chart rescales only when the peak crosses a step.
static float nice_max(float peak) {
    if (peak <= 0) return 1;
    float decade = powf(10, floorf(log10f(peak)));
    return peak <= decade ? decade : peak <= 2 * decade ? 2 * decade : peak <= 5 * decade ? 5 * decade : 10 * decade;
}

static float ring_max(const ring_t *r) {
    float m = 0;
    for (int i = 0; i < r->count; i++) m = fmaxf(m, ring_at(r, i));
    return m;
}

// 24 h history in 120 buckets of 12 minutes; recomputed only when the minute changes.
static void day_to(pitem_t *it, int series) {
    static uint32_t cached_minute[HS_COUNT];
    static float cavg[HS_COUNT][120], cmax[HS_COUNT][120];
    double now = CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970;
    uint32_t minute = (uint32_t)(now / 60);
    if (cached_minute[series] != minute) {
        static float avg[HISTORY_MINUTES], mx[HISTORY_MINUTES];
        history_series(series, now, avg, mx);
        for (int b = 0; b < 120; b++) {
            double sum = 0; float hi = -1; int n = 0;
            for (int i = b * 12; i < b * 12 + 12; i++)
                if (avg[i] == avg[i]) { sum += avg[i]; n++; if (mx[i] > hi) hi = mx[i]; }
            cavg[series][b] = n ? (float)(sum / n) / 100 : -1;
            cmax[series][b] = n ? hi / 100 : -1;
        }
        cached_minute[series] = minute;
    }
    memcpy(it->v[0], cavg[series], sizeof cavg[series]);
    memcpy(it->v[1], cmax[series], sizeof cmax[series]);
    it->n = 120;
}

static void build_panel_list(plist_t *l) {
    const snapshot_t *s = &S.snap;
    char a[48], b[48];
    pl_reset(l);
    int first = 1;
#define SECTION() do { if (!first) pl_divider(l); first = 0; } while (0)
    if (S.modules & (1u << M_CPU)) {
        SECTION();
        fmt_pct(a, s->cpu_total);
        pl_header(l, "CPU", a);
        pitem_t *sp = pl_spark(l, PC_BLUE, PC_BLUE);
        if (sp) ring_to(sp, 0, &S.hist[H_CPU], 1);
        pitem_t *dy = pl_day(l, PC_BLUE);
        if (dy) day_to(dy, HS_CPU);
        char line[96];
        static const float kCpuSlots[3] = {0, 70, 150};
        fmt_pct(a, s->cpu_user); fmt_pct(b, s->cpu_system);
        snprintf(line, sizeof line, "%s|%s|%.2f %.2f %.2f", a, b, s->load[0], s->load[1], s->load[2]);
        pl_kv(l, "user|system|load", line, NULL, kCpuSlots);
        if (s->ncores) {
            pitem_t *co = pl_cores(l);
            if (co) {
                co->n = (uint8_t)s->ncores;
                for (int i = 0; i < s->ncores; i++) { co->v[0][i] = s->core[i]; co->tag[i] = s->core_cluster[i] == s->nclusters - 1; }
            }
            if (s->nclusters > 1) {  // which bar color is which cluster (the last cluster is drawn orange)
                char names[96];
                names[0] = line[0] = 0;
                uint8_t col[4];
                int nk = s->nclusters < 4 ? s->nclusters : 4;
                for (int k = 0; k < nk; k++) {
                    fmt_pct(a, s->cluster_avg[k]);
                    if (k) { strlcat(names, "|", sizeof names); strlcat(line, "|", sizeof line); }
                    strlcat(names, s->cluster_name[k], sizeof names);
                    strlcat(line, a, sizeof line);
                    col[k] = k == s->nclusters - 1 ? PC_ORANGE : PC_BLUE;
                }
                pl_kv(l, names, line, col, NULL);
            }
        }
        static const char *const kThermal[] = {"", "Thermal state: fair", "Throttling: thermal state serious", "Throttling: thermal state critical"};
        if (S.thermal >= 1 && S.thermal <= 3) pl_note(l, kThermal[S.thermal]);
        if (S.procs_valid && S.ntop_cpu) {
            pl_gap(l, 4);
            float top = fmaxf(S.top_cpu[0].cpu, 1.0f);
            for (int i = 0; i < S.ntop_cpu; i++) {
                snprintf(b, sizeof b, "%.1f%%", S.top_cpu[i].cpu * 100);
                pl_proc(l, app_name(&S.top_cpu[i]), b, S.top_cpu[i].cpu / top, PC_BLUE);
            }
        }
        spike_t sp3[3];
        int nsp = history_spikes(CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970, 60, SPIKE_THRESHOLD * 100, sp3, 3);
        if (nsp) {
            pl_gap(l, 4);
            pl_caption(l, "Recent spikes");
            for (int i = 0; i < nsp; i++) {
                time_t tt = (time_t)sp3[i].when;
                struct tm tmv;
                localtime_r(&tt, &tmv);
                snprintf(a, sizeof a, "%02d:%02d  %s", tmv.tm_hour, tmv.tm_min, sp3[i].top);
                snprintf(b, sizeof b, "%.0f%%  %.1f cores", sp3[i].cpu_avg, sp3[i].top_share);
                pl_small(l, a, b);
            }
        }
    }
    if (S.modules & (1u << M_GPU)) {
        SECTION();
        if (s->gpu_ok) fmt_pct(a, s->gpu_util); else strcpy(a, "n/a");
        pl_header(l, "GPU", a);
        pitem_t *sp = pl_spark(l, PC_PURPLE, PC_PURPLE);
        if (sp) ring_to(sp, 0, &S.hist[H_GPU], 1);
        pitem_t *dy = pl_day(l, PC_PURPLE);
        if (dy) day_to(dy, HS_GPU);
        char line[96];
        fmt_pct(a, s->gpu_render); fmt_pct(b, s->gpu_tiler);
        snprintf(line, sizeof line, "%s|%s", a, b);
        pl_kv(l, "renderer|tiler", line, NULL, NULL);
        if (S.gpu_procs_valid && S.ntop_gpu) {
            pl_gap(l, 4);
            for (int i = 0; i < S.ntop_gpu; i++) {
                snprintf(b, sizeof b, "%.1f%%", S.top_gpu[i].cpu * 100);
                pl_proc(l, app_name(&S.top_gpu[i]), b, S.top_gpu[i].cpu, PC_PURPLE);
            }
        }
    }
    if (S.modules & (1u << M_MEM)) {
        SECTION();
        fmt_pct(a, mem_frac());
        pl_header(l, "Memory", a);
        pitem_t *sp = pl_spark(l, PC_GREEN, PC_GREEN);
        if (sp) ring_to(sp, 0, &S.hist[H_MEM], 1);
        pitem_t *dy = pl_day(l, PC_GREEN);
        if (dy) day_to(dy, HS_MEM);
        // app, wired, compressed and cached as one bar; the rest of the track is free
        double tot = s->mem_total ? (double)s->mem_total : 1;
        pitem_t *st = pl_stack(l);
        if (st) {
            const uint64_t part[4] = {s->mem_app, s->mem_wired, s->mem_compressed, s->mem_cache};
            const uint8_t col[4] = {PC_GREEN, PC_ORANGE, PC_PURPLE, PC_TEAL};
            st->n = 4;
            for (int i = 0; i < 4; i++) { st->v[0][i] = (float)(part[i] / tot); st->tag[i] = col[i]; }
        }
        char line[96], u[24], t[24];
        static const uint8_t kc1[2] = {PC_GREEN, PC_ORANGE}, kc2[2] = {PC_PURPLE, PC_TEAL};
        fmt_bytes(u, sizeof u, (double)s->mem_app); fmt_bytes(t, sizeof t, (double)s->mem_wired);
        snprintf(line, sizeof line, "%s|%s", u, t);
        pl_kv(l, "app|wired", line, kc1, NULL);
        fmt_bytes(u, sizeof u, (double)s->mem_compressed); fmt_bytes(t, sizeof t, (double)s->mem_cache);
        snprintf(line, sizeof line, "%s|%s", u, t);
        pl_kv(l, "compressed|cached", line, kc2, NULL);
        fmt_bytes(u, sizeof u, (double)s->mem_used); fmt_bytes(t, sizeof t, (double)s->mem_total);
        snprintf(line, sizeof line, "%s of %s", u, t);
        pl_kv(l, "used", line, NULL, NULL);
        if (s->swap_used > 0 || s->mem_pressure >= 2) {
            fmt_bytes(u, sizeof u, (double)s->swap_used);
            snprintf(line, sizeof line, "%s|%s", u, s->mem_pressure >= 4 ? "critical" : s->mem_pressure >= 2 ? "warning" : "normal");
            pl_kv(l, "swap|pressure", line, NULL, NULL);
        }
        if (S.procs_valid && S.ntop_mem) {
            pl_gap(l, 4);
            for (int i = 0; i < S.ntop_mem; i++) {
                fmt_bytes(b, sizeof b, (double)S.top_mem[i].memory);
                pl_proc(l, app_name(&S.top_mem[i]), b, (float)(S.top_mem[i].memory / tot), PC_GREEN);
            }
        }
    }
    if (S.modules & (1u << M_DISK)) {
        SECTION();
        fmt_pct(a, disk_used_frac());
        pl_header(l, "Disk", a);
        pl_bar(l, disk_used_frac(), PC_TEAL);
        char line[96], u[24], t[24], g[24];
        fmt_bytes(u, sizeof u, (double)s->disk_free); fmt_bytes(t, sizeof t, (double)s->disk_total);
        fmt_bytes(g, sizeof g, (double)s->disk_purgeable);
        static const float kDiskSlots[2] = {0, 170};
        snprintf(line, sizeof line, "%s of %s|%s", u, t, g);
        pl_kv(l, "free|purgeable", line, NULL, kDiskSlots);
        pitem_t *sp = pl_spark(l, PC_TEAL, PC_PINK);
        if (sp) {
            float mx = nice_max(fmaxf(fmaxf(ring_max(&S.hist[H_DISK_R]), ring_max(&S.hist[H_DISK_W])), 1e5f));
            ring_to(sp, 0, &S.hist[H_DISK_R], mx);
            ring_to(sp, 1, &S.hist[H_DISK_W], mx);
            sp->nseries = 2;
        }
        fmt_speed(a, sizeof a, s->disk_read_bps); fmt_speed(b, sizeof b, s->disk_write_bps);
        snprintf(line, sizeof line, "%s|%s", a, b);
        pl_kv(l, "read|write", line, NULL, NULL);
    }
    if (S.modules & (1u << M_NET)) {
        SECTION();
        pl_header(l, "Network", s->net_ok ? s->net_if : "offline");
        pitem_t *sp = pl_spark(l, PC_BLUE, PC_RED);
        if (sp) {
            float mx = nice_max(fmaxf(fmaxf(ring_max(&S.hist[H_NET_IN]), ring_max(&S.hist[H_NET_OUT])), 2e4f));
            ring_to(sp, 0, &S.hist[H_NET_IN], mx);
            ring_to(sp, 1, &S.hist[H_NET_OUT], mx);
            sp->nseries = 2;
        }
        char line[96];
        fmt_speed(a, sizeof a, s->net_in_bps); fmt_speed(b, sizeof b, s->net_out_bps);
        snprintf(line, sizeof line, "%s|%s", a, b);
        pl_kv(l, "down|up", line, NULL, NULL);
        fmt_bytes(a, sizeof a, (double)s->net_in_total); fmt_bytes(b, sizeof b, (double)s->net_out_total);
        snprintf(line, sizeof line, "%s|%s", a, b);
        pl_kv(l, "received|sent", line, NULL, NULL);
    }
    if (S.modules & (1u << M_SENSORS)) {
        SECTION();
        snprintf(a, sizeof a, "%.0f\xC2\xB0" "C", s->temp_cpu);
        pl_header(l, "Sensors", a);
        pitem_t *sp = pl_spark(l, PC_ORANGE, PC_ORANGE);
        if (sp) ring_to(sp, 0, &S.hist[H_TEMP], 110);
        snprintf(a, sizeof a, "%.1f\xC2\xB0" "C", s->temp_cpu); pl_row(l, "CPU", a);
        snprintf(a, sizeof a, "%.1f\xC2\xB0" "C", s->temp_gpu); pl_row(l, "GPU", a);
        for (int i = 0; i < s->nfans; i++) {
            char lab[16];
            snprintf(lab, sizeof lab, "Fan %d", i + 1);
            snprintf(a, sizeof a, "%.0f rpm", s->fan_rpm[i]);
            pl_row(l, lab, a);
        }
        snprintf(a, sizeof a, "%.1f W", s->power_system); pl_row(l, "System power", a);
    }
    if (S.modules & (1u << M_BLUETOOTH)) {
        SECTION();
        pl_header(l, "Bluetooth", s->nbt ? NULL : "none");
        for (int i = 0; i < s->nbt; i++) {
            fmt_pct(a, s->bt[i].level);
            if (s->bt[i].charging) strlcat(a, " charging", sizeof a);
            pl_row(l, s->bt[i].name, a);
            pl_bar(l, s->bt[i].level, s->bt[i].level < 0.2f ? PC_RED : PC_GREEN);
        }
        if (!s->nbt) pl_caption(l, "Connected accessories report their battery here");
    }
    if (S.modules & (1u << M_CLOCK)) {
        SECTION();
        static NSDateFormatter *tf, *df;
        static NSMutableArray<NSDateFormatter *> *zf;
        static NSArray<NSString *> *zones;
        if (!tf) {
            tf = [NSDateFormatter new];
            tf.timeStyle = NSDateFormatterMediumStyle;
            df = [NSDateFormatter new];
            df.dateStyle = NSDateFormatterFullStyle;
            zones = [NSUserDefaults.standardUserDefaults stringArrayForKey:@"clockZones"] ?: @[ @"UTC" ];
            zf = [NSMutableArray array];
            for (NSString *z in zones) {
                NSDateFormatter *f = [NSDateFormatter new];
                f.timeStyle = NSDateFormatterShortStyle;
                f.timeZone = [NSTimeZone timeZoneWithName:z] ?: NSTimeZone.localTimeZone;
                [zf addObject:f];
            }
        }
        NSDate *now = [NSDate date];
        pl_header(l, "Clock", [tf stringFromDate:now].UTF8String);
        pl_row(l, "Date", [df stringFromDate:now].UTF8String);
        for (NSUInteger i = 0; i < zf.count; i++)
            pl_row(l, [zones[i] componentsSeparatedByString:@"/"].lastObject.UTF8String, [zf[i] stringFromDate:now].UTF8String);
    }
    if ((S.modules & (1u << M_BATTERY)) && s->batt_present) {
        SECTION();
        fmt_pct(a, s->batt_level);
        pl_header(l, "Battery", a);
        pl_bar(l, s->batt_level, PC_GREEN);
        pl_row(l, "Source", s->batt_ac ? "Power adapter" : "Battery");
        pl_row(l, "State", s->batt_charging ? "Charging" : "Not charging");
        if (s->batt_minutes > 0) snprintf(a, sizeof a, "%d:%02d", s->batt_minutes / 60, s->batt_minutes % 60);
        else strcpy(a, "-");
        pl_row(l, s->batt_charging ? "Time to full" : "Time remaining", a);
    }
    pl_gap(l, 4);
    pl_caption(l, "Right-click the menu bar item for settings");
#undef SECTION
}

static int gPanelDark = -1;  // the panel's appearance, cached until it changes
@interface LeanPanelView : NSView
@end
@implementation LeanPanelView {
@public
    CALayer *_static, *_content;  // labels/backgrounds below, values/chart ink above
}
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        for (int i = 0; i < 2; i++) {
            CALayer *l = [CALayer layer];
            l.anchorPoint = CGPointZero;
            l.frame = self.bounds;
            l.contentsGravity = kCAGravityTopLeft;
            l.actions = @{@"contents" : NSNull.null, @"bounds" : NSNull.null, @"position" : NSNull.null};
            [self.layer addSublayer:l];
            if (i == 0) _static = l; else _content = l;
        }
    }
    return self;
}
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {}
- (void)viewDidChangeEffectiveAppearance { gPanelDark = -1; panel_colors(self); if (S.panel_open) panel_refresh(); }
- (void)scrollWheel:(NSEvent *)e {
    gPanelScroll -= (float)(e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 10));
    panel_refresh();
}
@end

static LeanPanelView *gLeanView;

#if LITE_DEV
// LITE_DUMP=<dir>: write the live menu bar strip and the composited panel (static
// under dynamic layer) to PNGs once, for visual verification without screen capture.
static void dump_surface_png(IOSurfaceRef a, IOSurfaceRef b, const char *path, uint32_t backdrop) {
    if (!a) return;
    IOSurfaceLock(a, kIOSurfaceLockReadOnly, NULL);
    if (b) IOSurfaceLock(b, kIOSurfaceLockReadOnly, NULL);
    size_t w = IOSurfaceGetWidth(a), h = IOSurfaceGetHeight(a);
    uint32_t *out = malloc(w * h * 4);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            uint32_t pa = ((uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(a) + y * IOSurfaceGetBytesPerRow(a)))[x];
            uint32_t pb = b ? ((uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(b) + y * IOSurfaceGetBytesPerRow(b)))[x] : 0;
            uint32_t px = pb ? blit_blend(pb, pa) : pa;
            out[y * w + x] = blit_blend(px, backdrop);  // opaque backdrop like the real popover/menu bar
        }
    if (b) IOSurfaceUnlock(b, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceUnlock(a, kIOSurfaceLockReadOnly, NULL);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(out, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(c);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
    CGImageRelease(img);
    CGContextRelease(c);
    CGColorSpaceRelease(cs);
    free(out);
}

#endif

static void lean_panel_render(void) {
    NSRect b = gLeanView.bounds;
    float content = panel_measure(&gPL);
    float maxs = fmaxf(0, content - (float)b.size.height);
    gPanelScroll = fminf(fmaxf(gPanelScroll, 0), maxs);
    if (gPanelDark < 0) {  // reset by viewDidChangeEffectiveAppearance
        NSAppearance *ap = gLeanView.effectiveAppearance;
        gPanelDark = [[ap bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]] isEqualToString:NSAppearanceNameDarkAqua];
    }
    int dark = gPanelDark;
    float scale = (float)(gPanel.backingScaleFactor > 0 ? gPanel.backingScaleFactor : 2);
    int painted = 0;
    IOSurfaceRef st = NULL;
    IOSurfaceRef sf = panel_render(&gPR, &gPL, (float)b.size.height, scale, dark, gPanelScroll, &painted, &st);
#if LITE_DEV
    if (gProf > 0) {
        static int frames, rows, scrolled, kinds[16];
        frames++; rows += painted; scrolled += gPR.last_scrolled;
        for (int i = 0; i < gPL.n; i++) if (gPR.changed[i] == gPR.frame) kinds[gPL.item[i].kind]++;
        if (frames == 30) {
            fprintf(stderr, "panel: %.1f rows/frame repainted, %.1f sparks scrolled; changed by kind:", rows / 30.0, scrolled / 30.0);
            for (int k = 0; k < 14; k++) fprintf(stderr, " %d:%.1f", k, kinds[k] / 30.0);
            fprintf(stderr, " (of %d rows); full repaints %u, static repaints %u so far\n", gPL.n, gPR.full_repaints, gPR.static_repaints);
            frames = rows = scrolled = 0;
            memset(kinds, 0, sizeof kinds);
        }
    }
#endif
    if (!sf && !st) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (st) {
        gLeanView->_static.frame = b;
        gLeanView->_static.contentsScale = scale;
        gLeanView->_static.contents = (__bridge id)st;
    }
    if (sf) {
        gLeanView->_content.frame = b;
        gLeanView->_content.contentsScale = scale;
        gLeanView->_content.contents = (__bridge id)sf;
    }
    [CATransaction commit];
#if LITE_DEV
    static int dumped;
    static const char *dump;
    if (!dumped) dump = getenv("LITE_DUMP");
    if (dump && ++dumped == 90) {  // after 90 frames, so the charts carry real history
        int dk = S.env_dark;
        uint32_t bd = dk ? 0xff2a2a2cu : 0xffececeeu;
        char path[1024];
        snprintf(path, sizeof path, "%s/live-panel.png", dump);
        dump_surface_png((__bridge IOSurfaceRef)gLeanView->_static.contents, (__bridge IOSurfaceRef)gLeanView->_content.contents, path, bd);
        snprintf(path, sizeof path, "%s/live-strip.png", dump);
        dump_surface_png((__bridge IOSurfaceRef)gContent.contents, NULL, path, dk ? 0xff1e1e20u : 0xfff2f2f4u);
    }
#endif
    if (st) CFRelease(st);
    if (sf) CFRelease(sf);
}

static const CGFloat W = PANEL_W;  // panel width in points

// Recomputed only when the content height, the item's position or the screen changes.
static int gPanelPosDirty = 1;
static float gPanelPosH = -1;
static void panel_position(void) {
    NSWindow *bw = gItem.button.window;
    if (!bw) return;
    float mh = panel_measure(&gPL);
    if (!gPanelPosDirty && mh == gPanelPosH) return;
    gPanelPosDirty = 0;
    gPanelPosH = mh;
    CGFloat h = mh;
    NSRect sf = bw.screen.visibleFrame;
    NSRect br = bw.frame;
    h = fmin(h, NSMaxY(sf) - NSMinY(sf) - 12);  // never taller than the screen; scroll instead
    CGFloat x = NSMidX(br) - W / 2;
    x = fmax(NSMinX(sf) + 6, fmin(x, NSMaxX(sf) - W - 6));
    NSRect f = NSMakeRect(x, NSMinY(br) - h - 4, W, h);
    if (!NSEqualRects(gPanel.frame, f)) [gPanel setFrame:f display:NO];
}

static void panel_refresh(void) {
    if (!S.panel_open) return;
    // Top processes refresh every 2 s, and immediately after opening.
    static uint32_t procs_step;
    int procs_due = !S.procs_valid || S.step - procs_step >= 2;
    if (procs_due && (S.modules & ((1u << M_CPU) | (1u << M_MEM)))) {
        procs_set_busy_cores(S.snap.cpu_total * (float)NSProcessInfo.processInfo.activeProcessorCount);
        S.procs_valid = procs_sample(2, S.top_cpu, &S.ntop_cpu, S.top_mem, &S.ntop_mem, 5);
        procs_step = S.step;
    }
    static uint32_t gpu_procs_step;
    if ((!S.gpu_procs_valid || S.step - gpu_procs_step >= 2) && (S.modules & (1u << M_GPU))) {
        S.gpu_procs_valid = gpuprocs_sample(S.snap.gpu_ok ? S.snap.gpu_util : -1, S.top_gpu, &S.ntop_gpu, 5);
        gpu_procs_step = S.step;
    }
    prof_mark(3);
    build_panel_list(&gPL);
    prof_mark(4);
    panel_position();
    prof_mark(5);
    lean_panel_render();
    prof_mark(6);
}

static void panel_close(void) {
    if (!S.panel_open) return;
    S.panel_open = 0;
    schedule_timer(0);  // back to the menu bar cadence
    [gPanel orderOut:nil];
    gItem.button.highlighted = NO;
    if (gGlobalMonitor) { [NSEvent removeMonitor:gGlobalMonitor]; gGlobalMonitor = nil; }
    procs_reset();
    gpuprocs_reset();
    S.procs_valid = S.gpu_procs_valid = 0;
    S.ntop_cpu = S.ntop_mem = S.ntop_gpu = 0;
    memset(gNames, 0, sizeof gNames);
    gLeanView->_static.contents = nil;  // let the layers drop their surfaces too
    gLeanView->_content.contents = nil;
    panel_release(&gPR);  // free the surfaces while closed
    memset(S.snap.core, 0, sizeof(S.snap.core));
    S.snap.ncores = 0;
}

static void panel_open(void) {
    if (!gPanel) {
        gPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, W, 400)
                                            styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                              backing:NSBackingStoreBuffered
                                                defer:YES];
        gPanel.level = NSPopUpMenuWindowLevel;
        gPanel.opaque = NO;
        gPanel.backgroundColor = NSColor.clearColor;
        gPanel.hasShadow = YES;
        gPanel.hidesOnDeactivate = NO;
        gPanel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorTransient |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary;
        NSVisualEffectView *fx = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, W, 400)];
        fx.material = NSVisualEffectMaterialPopover;
        fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        fx.state = NSVisualEffectStateActive;
        fx.wantsLayer = YES;
        fx.layer.cornerRadius = 12;
        fx.layer.masksToBounds = YES;
        fx.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        gPanel.contentView = fx;
        panel_init(&gPR);
        gLeanView = [[LeanPanelView alloc] initWithFrame:fx.bounds];
        gLeanView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [fx addSubview:gLeanView];
        panel_colors(gLeanView);
    }
    S.panel_open = 1;
    gPanelScroll = 0;
    gItem.button.highlighted = YES;
    schedule_timer(1);  // panel cadence; fires a fresh tick with every panel metric
    gPanelPosDirty = 1;
    panel_position();
    [gPanel orderFrontRegardless];
    gGlobalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
                                                            handler:^(NSEvent *e) { LITE_LOG(@"close: global %@", e); panel_close(); }];
}

// ---- Timer / tick ----------------------------------------------------------------------
static int suspended(void) { return S.screens_asleep || S.locked || S.inactive || S.system_asleep || (S.occluded && !S.panel_open); }

// ---- Threshold notifications (opt-in; checks are comparisons on values already sampled)
typedef enum { A_CPU, A_MEM, A_DISK, A_BATT, A_COUNT } alert_t;
static NSString *const kAlertKey[A_COUNT] = {@"alert.cpu", @"alert.memory", @"alert.disk", @"alert.battery"};
static NSString *const kAlertTitle[A_COUNT] = {@"CPU above 90% for 30 s", @"Memory pressure critical for 10 s",
                                                @"Disk space below 10%", @"Battery at 20% or less"};
static struct { int enabled[A_COUNT]; double since[A_COUNT], last_fired[A_COUNT]; } gAlerts;

static void alert_post(alert_t a, NSString *body) {
    double now = CFAbsoluteTimeGetCurrent();
    if (now - gAlerts.last_fired[a] < 15 * 60) return;  // cooldown
    gAlerts.last_fired[a] = now;
    UNMutableNotificationContent *c = [UNMutableNotificationContent new];
    c.title = @"Stats Lite";
    c.body = body;
    UNNotificationRequest *r = [UNNotificationRequest requestWithIdentifier:kAlertKey[a] content:c trigger:nil];
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:r withCompletionHandler:nil];
}

// cond held continuously for `hold` seconds -> notify
static void alert_check(alert_t a, int cond, double hold, NSString *body) {
    if (!gAlerts.enabled[a]) return;
    double now = CFAbsoluteTimeGetCurrent();
    if (!cond) { gAlerts.since[a] = 0; return; }
    if (!gAlerts.since[a]) gAlerts.since[a] = now;
    if (now - gAlerts.since[a] >= hold) alert_post(a, body);
}

static void alerts_tick(unsigned need) {
    const snapshot_t *s = &S.snap;
    if (need & NEED_CPU) alert_check(A_CPU, s->cpu_total >= 0.9f, 30, [NSString stringWithFormat:@"CPU has been above 90%% for 30 seconds (now %d%%).", (int)lroundf(s->cpu_total * 100)]);
    if (need & NEED_MEM) alert_check(A_MEM, s->mem_pressure >= 4, 10, @"Memory pressure is critical.");
    if ((need & NEED_DISK_SPACE) && s->disk_total)
        alert_check(A_DISK, (double)s->disk_free / (double)s->disk_total < 0.10, 0, [NSString stringWithFormat:@"Less than 10%% of disk space is free (%.0f GB).", s->disk_free / 1e9]);
}

// Spike attribution: a baseline process scan at the first busy tick of a minute; when
// that minute ends with an average over SPIKE_THRESHOLD, a second scan names the top process.
static void attribute_spikes(double unix_now) {
    uint32_t minute = (uint32_t)(unix_now / 60.0);
    if (minute != S.attr_minute) {
        if (S.attr_baseline && S.attr_n && S.attr_sum / S.attr_n >= SPIKE_THRESHOLD) {
            char name[24];
            float share;
            if (procs_top_since_last(name, sizeof name, &share)) history_set_top(S.attr_minute * 60.0 + 1, name, share);
        } else {
            S.attr_baseline = 0;
        }
        S.attr_minute = minute;
        S.attr_sum = 0;
        S.attr_n = 0;
    }
    S.attr_sum += S.snap.cpu_total;
    S.attr_n++;
    // A minute can only average SPIKE_THRESHOLD if some tick reaches it, so an idle machine never scans.
    if (!S.attr_baseline && S.snap.cpu_total >= SPIKE_THRESHOLD) {
        char unused[2];
        float sh;
        procs_top_since_last(unused, sizeof unused, &sh);
        S.attr_baseline = 1;
    }
}

static void tick(void) {
#if LITE_DEV
    if (gProf < 0) gProf = getenv("LITE_PROFILE") != NULL;
#endif
    prof_mark(-1);
    double now = CFAbsoluteTimeGetCurrent();
    int fresh = S.last_tick <= 0;  // first tick after (re)start: sample everything
    double dt = fresh ? base_period() : now - S.last_tick;
    S.last_tick = now;
    unsigned need = module_need_at(S.step, S.force_all || fresh);
    S.step += (uint32_t)base_period();
    S.force_all = 0;
    static int sampled_once;
    if (!(S.bench_skip & 1) || !sampled_once) sampler_collect(need, dt, &S.snap);
    sampled_once = 1;
    if (need & NEED_DISK_SPACE) sampler_refresh_purgeable(300);
    if (need & NEED_BT) sampler_bluetooth(&S.snap);
    prof_mark(0);

    // Rings advance only when their metric was sampled, so charts never repeat values.
    if (need & NEED_CPU) { ring_push(&S.hist[H_CPU], S.snap.cpu_total); ring_push(&S.hist[H_CPU_SYS], S.snap.cpu_system); }
    if (need & NEED_GPU) ring_push(&S.hist[H_GPU], S.snap.gpu_util);
    if (need & NEED_MEM) ring_push(&S.hist[H_MEM], mem_frac());
    if (need & NEED_DISK_IO) { ring_push(&S.hist[H_DISK_R], (float)S.snap.disk_read_bps); ring_push(&S.hist[H_DISK_W], (float)S.snap.disk_write_bps); }
    if (need & NEED_NET) { ring_push(&S.hist[H_NET_IN], (float)S.snap.net_in_bps); ring_push(&S.hist[H_NET_OUT], (float)S.snap.net_out_bps); }
    if (need & NEED_SENSORS) ring_push(&S.hist[H_TEMP], S.snap.temp_cpu);

    float hv[HS_COUNT] = {S.snap.cpu_total * 100, S.snap.gpu_util * 100, mem_frac() * 100, disk_used_frac() * 100,
                          (float)S.snap.net_in_bps, (float)S.snap.net_out_bps, S.snap.temp_cpu};
    unsigned hm = ((need & NEED_CPU) ? 1u << HS_CPU : 0) | ((need & NEED_GPU) && S.snap.gpu_ok ? 1u << HS_GPU : 0) |
                  ((need & NEED_MEM) ? 1u << HS_MEM : 0) | ((need & NEED_DISK_SPACE) ? 1u << HS_DISK : 0) |
                  ((need & NEED_NET) ? (1u << HS_NET_IN) | (1u << HS_NET_OUT) : 0) |
                  ((need & NEED_SENSORS) ? 1u << HS_TEMP : 0);
    history_add(now + kCFAbsoluteTimeIntervalSince1970, hv, hm);
    if (need & NEED_CPU) attribute_spikes(now + kCFAbsoluteTimeIntervalSince1970);
    if (gAlerts.enabled[A_CPU] | gAlerts.enabled[A_MEM] | gAlerts.enabled[A_DISK]) alerts_tick(need);
    prof_mark(1);

    update_strip(0);
    prof_mark(2);
    if (S.panel_open) panel_refresh();
    prof_report();
}

static void tick_sample_only(void) {
    unsigned need = module_need();
    sampler_collect(need, 0.1, &S.snap);
    if (need & NEED_CPU) { ring_push(&S.hist[H_CPU], S.snap.cpu_total); ring_push(&S.hist[H_CPU_SYS], S.snap.cpu_system); }
    if (need & NEED_GPU) ring_push(&S.hist[H_GPU], S.snap.gpu_util);
    if (need & NEED_MEM) ring_push(&S.hist[H_MEM], mem_frac());
    if (need & NEED_NET) { ring_push(&S.hist[H_NET_IN], (float)S.snap.net_in_bps); ring_push(&S.hist[H_NET_OUT], (float)S.snap.net_out_bps); }
}

static void request_tick(void) { @autoreleasepool { S.last_tick = 0; tick(); } }
static void request_redraw(void) { @autoreleasepool { update_strip(1); } }

static void timer_cb(CFRunLoopTimerRef t, void *info) {
    @autoreleasepool { tick(); }
}

static void schedule_timer(int fire_now) {
    if (S.timer) { CFRunLoopTimerInvalidate(S.timer); CFRelease(S.timer); S.timer = NULL; }
    if (S.dtimer) { dispatch_source_cancel(S.dtimer); S.dtimer = nil; }
    if (suspended()) return;
    if (fire_now) request_tick();
    if (S.timer_kind == 2) {
        S.dtimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        double iv = base_period();
        dispatch_source_set_timer(S.dtimer, dispatch_walltime(NULL, (int64_t)(iv * NSEC_PER_SEC)),
                                  (uint64_t)(iv * NSEC_PER_SEC), (uint64_t)(iv * S.tolerance * NSEC_PER_SEC));
        dispatch_source_set_event_handler(S.dtimer, ^{ tick(); });
        dispatch_resume(S.dtimer);
        return;
    }
    // Align to wall-clock second boundaries so we coalesce with the many other
    // per-second timers on the system; generous tolerance lets the kernel batch.
    double iv = base_period();
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CFAbsoluteTime first = S.timer_kind == 1 ? now + 1 : floor(now) + 1;
    if (fmod(iv, 60) == 0) {  // per-minute cadence (e.g. a clock alone): fire on the minute
        double unix_now = now + kCFAbsoluteTimeIntervalSince1970;
        first = (floor(unix_now / 60) + 1) * 60 - kCFAbsoluteTimeIntervalSince1970 + 0.05;
    }
    S.timer = CFRunLoopTimerCreate(NULL, first, iv, 0, 0, timer_cb, NULL);
    CFRunLoopTimerSetTolerance(S.timer, fmin(iv * S.tolerance, 1.0));  // a clock may be at most 1 s late
    CFRunLoopAddTimer(CFRunLoopGetMain(), S.timer, kCFRunLoopCommonModes);
}

static void suspend_changed(void) {
    sampler_reprime_rates();
    // A gap in sampling (sleep, lock, occlusion) breaks the per-minute attribution window.
    S.attr_baseline = 0;
    S.attr_n = 0;
    S.attr_sum = 0;
    schedule_timer(!suspended());
}

// ---- Event sources ----------------------------------------------------------------------
static void battery_cb(void *ctx) {
    if (S.modules & (1u << M_BATTERY) || gAlerts.enabled[A_BATT]) sampler_battery(&S.snap);
    if (gAlerts.enabled[A_BATT] && S.snap.batt_present)
        alert_check(A_BATT, S.snap.batt_level <= 0.2f && !S.snap.batt_charging && !S.snap.batt_ac, 0,
                    [NSString stringWithFormat:@"Battery is at %d%%.", (int)lroundf(S.snap.batt_level * 100)]);
    if (S.modules & (1u << M_BLUETOOTH)) sampler_bluetooth(&S.snap);
    if (suspended()) return;
    update_strip(0);
    if (S.panel_open) panel_refresh();
}

static void net_cb(SCDynamicStoreRef store, CFArrayRef keys, void *info) { sampler_net_invalidate(); }

static void apply_modules(void) {
    // Battery: event-driven, only while shown.
    int want_batt = (S.modules & ((1u << M_BATTERY) | (1u << M_BLUETOOTH))) != 0 || gAlerts.enabled[A_BATT];  // IOPS events
    if (want_batt && !gBatterySource) {
        gBatterySource = IOPSNotificationCreateRunLoopSource(battery_cb, NULL);
        if (gBatterySource) CFRunLoopAddSource(CFRunLoopGetMain(), gBatterySource, kCFRunLoopDefaultMode);
        sampler_battery(&S.snap);
    } else if (!want_batt && gBatterySource) {
        CFRunLoopSourceInvalidate(gBatterySource);
        CFRelease(gBatterySource);
        gBatterySource = NULL;
    }
    // Network: primary interface changes are pushed by configd.
    int want_net = (S.modules & (1u << M_NET)) != 0;
    if (want_net && !gNetStore) {
        gNetStore = SCDynamicStoreCreate(NULL, CFSTR("StatsLite"), net_cb, NULL);
        CFStringRef k[2] = {CFSTR("State:/Network/Global/IPv4"), CFSTR("State:/Network/Global/IPv6")};
        CFArrayRef keys = CFArrayCreate(NULL, (const void **)k, 2, &kCFTypeArrayCallBacks);
        SCDynamicStoreSetNotificationKeys(gNetStore, keys, NULL);
        CFRelease(keys);
        gNetSource = SCDynamicStoreCreateRunLoopSource(NULL, gNetStore, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), gNetSource, kCFRunLoopDefaultMode);
        sampler_net_invalidate();
    } else if (!want_net && gNetStore) {
        CFRunLoopSourceInvalidate(gNetSource);
        CFRelease(gNetSource);
        CFRelease(gNetStore);
        gNetSource = NULL;
        gNetStore = NULL;
    }
}

// ---- Preferences --------------------------------------------------------------------
static void load_prefs(void) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSString *mods = [d stringForKey:@"modules"] ?: @"cpu,gpu,mem,disk";
    S.modules = 0;
    for (NSString *k in [mods componentsSeparatedByString:@","])
        for (int m = 0; m < M_COUNT; m++)
            if ([k isEqualToString:kModuleKey[m]]) S.modules |= 1u << m;
    if (!S.modules) S.modules = 1u << M_CPU;
    for (int m = 0; m < M_COUNT; m++) {
        NSString *v = [d stringForKey:[@"style." stringByAppendingString:kModuleKey[m]]];
        S.style[m] = STYLE_TEXT;
        for (int k = 1; k < 5; k++) if ([v isEqualToString:kStyleName[k]]) S.style[m] = (uint8_t)k;
    }
    // Optional legacy/bench override: "interval" N sets CPU, GPU, memory, network and disk activity to N s.
    NSInteger iv = [d integerForKey:@"interval"];
    for (int m = 0; m < M_COUNT; m++) {
        NSInteger e = [d integerForKey:[@"every." stringByAppendingString:kModuleKey[m]]];
        gEvery[m] = e >= 1 && e <= 240 ? (uint8_t)e : kEveryDefault[m];
        if (iv >= 1 && iv <= 60 && (m == M_CPU || m == M_GPU || m == M_MEM || m == M_NET)) gEvery[m] = (uint8_t)iv;
    }
    NSInteger eio = [d integerForKey:@"every.diskio"];
    gEveryDiskIO = eio >= 1 && eio <= 60 ? (uint8_t)eio : (iv >= 1 && iv <= 60 ? (uint8_t)iv : 1);
    S.labels = [d objectForKey:@"labels"] ? [d boolForKey:@"labels"] : 1;
    S.mono = [d boolForKey:@"monochromeCharts"];
    for (int a = 0; a < A_COUNT; a++) gAlerts.enabled[a] = [d boolForKey:kAlertKey[a]];
    S.bench_skip = (int)[d integerForKey:@"benchSkip"];
    S.timer_kind = (int)[d integerForKey:@"benchTimerKind"];
    S.tolerance = [d objectForKey:@"benchTolerance"] ? [d doubleForKey:@"benchTolerance"] : 0.1;
    S.renderer.mode = [d objectForKey:@"renderMode"] ? (int)[d integerForKey:@"renderMode"] : RENDER_BLIT_SURFACE;
}

static void save_prefs(void) {
    NSMutableArray *a = [NSMutableArray array];
    for (int m = 0; m < M_COUNT; m++) if (S.modules & (1u << m)) [a addObject:kModuleKey[m]];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:[a componentsJoinedByString:@","] forKey:@"modules"];
    for (int m = 0; m < M_COUNT; m++) [d setInteger:gEvery[m] forKey:[@"every." stringByAppendingString:kModuleKey[m]]];
    [d setInteger:gEveryDiskIO forKey:@"every.diskio"];
    [d removeObjectForKey:@"interval"];
    [d setBool:S.labels forKey:@"labels"];
    [d setBool:S.mono forKey:@"monochromeCharts"];
    for (int m = 0; m < M_COUNT; m++) [d setObject:kStyleName[S.style[m] ? S.style[m] : 1] forKey:[@"style." stringByAppendingString:kModuleKey[m]]];
}


#if LITE_DEV
// ---- Self-test (make DEV=1; StatsLite -selfTest YES [-selfTestDir DIR]) -------------
// Drives the same handlers the status item click and the settings menu call, and
// dumps the live menu bar / panel surfaces after each step. Prints a summary, exits.
static int gStFail, gStStep;
static NSString *gStDir;
static void st_check(int ok, NSString *what) {
    printf("[%s] %s\n", ok ? " ok " : "FAIL", what.UTF8String);
    if (!ok) gStFail++;
}
// The panel hangs just below the status item, as tall as its content (or the screen).
static int st_panel_placed(void) {
    NSWindow *bw = gItem.button.window;
    NSRect f = gPanel.frame, br = bw.frame, sf = bw.screen.visibleFrame;
    CGFloat want = fmin(panel_measure(&gPL), NSMaxY(sf) - NSMinY(sf) - 12);
    return fabs(NSMaxY(f) - (NSMinY(br) - 4)) < 0.5 && fabs(f.size.height - want) < 0.5 &&
           NSMinX(f) >= NSMinX(sf) && NSMaxX(f) <= NSMaxX(sf);
}
static void st_dump(NSString *name) {
    int dk = S.env_dark;
    char path[1024];
    snprintf(path, sizeof path, "%s/%02d-%s-strip.png", gStDir.fileSystemRepresentation, gStStep, name.UTF8String);
    dump_surface_png((__bridge IOSurfaceRef)gContent.contents, NULL, path, dk ? 0xff1e1e20u : 0xfff2f2f4u);
    if (S.panel_open && gLeanView) {
        snprintf(path, sizeof path, "%s/%02d-%s-panel.png", gStDir.fileSystemRepresentation, gStStep, name.UTF8String);
        dump_surface_png((__bridge IOSurfaceRef)gLeanView->_static.contents, (__bridge IOSurfaceRef)gLeanView->_content.contents, path,
                         dk ? 0xff2a2a2cu : 0xffececeeu);
    }
    gStStep++;
}
static NSMenu *gStMenu;  // the settings menu while it is open (self-test)

// A real mouse click delivered through AppKit's event dispatch (hit testing, the
// status bar button's tracking loop, target/action), at the status item's center.
static void st_click(int right) {
    NSStatusBarButton *b = gItem.button;
    NSWindow *w = b.window;
    NSPoint p = [b convertPoint:NSMakePoint(NSMidX(b.bounds), NSMidY(b.bounds)) toView:nil];
    NSEventType dn = right ? NSEventTypeRightMouseDown : NSEventTypeLeftMouseDown, up = right ? NSEventTypeRightMouseUp : NSEventTypeLeftMouseUp;
    NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
    NSEvent *down = [NSEvent mouseEventWithType:dn location:p modifierFlags:0 timestamp:t windowNumber:w.windowNumber context:nil
                                    eventNumber:1 clickCount:1 pressure:1];
    NSEvent *release = [NSEvent mouseEventWithType:up location:p modifierFlags:0 timestamp:t + 0.05 windowNumber:w.windowNumber context:nil
                                       eventNumber:2 clickCount:1 pressure:0];
    [NSApp postEvent:release atStart:NO];  // picked up by the button's tracking loop
    [NSApp sendEvent:down];
}

static NSMenuItem *st_item(NSInteger tag) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    it.tag = tag;
    return it;
}
#endif
// ---- Hover tooltip: built only when AppKit asks (pointer resting on the item) --------
// Highest sample in a ring and how many samples ago it was.
static float ring_peak(const ring_t *r, int *ago) {
    float best = -1;
    *ago = 0;
    for (int i = 0; i < r->count; i++) {
        float v = ring_at(r, i);
        if (v >= best) { best = v; *ago = r->count - 1 - i; }
    }
    return best;
}
static void fmt_ago(char *out, size_t cap, int seconds) {
    if (seconds < 2) snprintf(out, cap, "now");
    else if (seconds < 90) snprintf(out, cap, "%d s ago", seconds);
    else snprintf(out, cap, "%d min ago", (seconds + 30) / 60);
}
static NSString *tooltip_text(void) {
    char out[1024], ago[24], a[32], b[32];
    size_t n = 0;
    out[0] = 0;
#define TT(...) do { if (n < sizeof out) n += (size_t)snprintf(out + n, sizeof out - n, __VA_ARGS__); } while (0)
    const snapshot_t *s = &S.snap;
    int ag;
    if (S.modules & (1u << M_CPU) && S.hist[H_CPU].count) {
        float pk = ring_peak(&S.hist[H_CPU], &ag);
        fmt_ago(ago, sizeof ago, ag * module_period(M_CPU));
        TT("CPU %.0f%% (user %.0f%%, system %.0f%%), peak %.0f%% %s\n", s->cpu_total * 100, (s->cpu_total - s->cpu_system) * 100,
           s->cpu_system * 100, pk * 100, ago);
    }
    if (S.modules & (1u << M_GPU) && S.hist[H_GPU].count && s->gpu_ok) {
        float pk = ring_peak(&S.hist[H_GPU], &ag);
        fmt_ago(ago, sizeof ago, ag * module_period(M_GPU));
        TT("GPU %.0f%%, peak %.0f%% %s\n", s->gpu_util * 100, pk * 100, ago);
    }
    if (S.modules & (1u << M_MEM) && S.hist[H_MEM].count) {
        float pk = ring_peak(&S.hist[H_MEM], &ag);
        fmt_ago(ago, sizeof ago, ag * module_period(M_MEM));
        static const char *const kPressure[] = {"", "normal", "warning", "", "critical"};
        fmt_bytes(a, sizeof a, (double)s->mem_used);
        TT("Memory %.0f%% (%s used), pressure %s, peak %.0f%% %s\n", mem_frac() * 100, a,
           s->mem_pressure >= 1 && s->mem_pressure <= 4 ? kPressure[s->mem_pressure] : "normal", pk * 100, ago);
    }
    if (S.modules & (1u << M_DISK) && s->disk_total) {
        fmt_bytes(a, sizeof a, (double)s->disk_free);
        fmt_bytes(b, sizeof b, (double)s->disk_total);
        TT("Disk %s free of %s\n", a, b);
    }
    if (S.modules & (1u << M_NET) && S.hist[H_NET_IN].count) {
        int agi, ago2;
        float pi = ring_peak(&S.hist[H_NET_IN], &agi), po = ring_peak(&S.hist[H_NET_OUT], &ago2);
        fmt_speed(a, sizeof a, s->net_in_bps);
        fmt_speed(b, sizeof b, s->net_out_bps);
        TT("Network down %s, up %s", a, b);
        fmt_speed(a, sizeof a, pi);
        fmt_ago(ago, sizeof ago, agi * module_period(M_NET));
        TT(", peak down %s %s\n", a, ago);
        (void)po; (void)ago2;
    }
    spike_t sp;
    if (history_spikes(CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970, 60, SPIKE_THRESHOLD * 100, &sp, 1) == 1) {
        time_t tt = (time_t)sp.when;
        struct tm tmv;
        localtime_r(&tt, &tmv);
        TT("Busiest recent minute: %02d:%02d, CPU %.0f%%, mostly %s\n", tmv.tm_hour, tmv.tm_min, sp.cpu_avg, sp.top);
    }
#undef TT
    while (n > 0 && n <= sizeof out && out[n - 1] == '\n') out[--n] = 0;
    return n ? [NSString stringWithUTF8String:out] : @"Stats Lite";
}

// ---- App delegate ------------------------------------------------------------------
@interface App : NSObject <NSApplicationDelegate, NSMenuDelegate, NSViewToolTipOwner>
@end

@implementation App
// The app was called Stats Lean: carry its settings and history over once.
static void migrate_old_name(void) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSString *bid = NSBundle.mainBundle.bundleIdentifier;
    if (bid && ![d persistentDomainForName:bid]) {
        NSDictionary *old = [d persistentDomainForName:@"dev.statslean.app"];
        if (old.count) [d setPersistentDomain:old forName:bid];
    }
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *base = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString *from = [base stringByAppendingPathComponent:@"StatsLean"], *to = [base stringByAppendingPathComponent:@"StatsLite"];
    if (![fm fileExistsAtPath:to] && [fm fileExistsAtPath:from]) [fm moveItemAtPath:from toPath:to error:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    renderer_init(&S.renderer);
    migrate_old_name();
    load_prefs();
    sampler_init();
    NSString *dir = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"StatsLite"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    // -historyPath lets benchmark copies run side by side without sharing the file.
    NSString *hp = [NSUserDefaults.standardUserDefaults stringForKey:@"historyPath"] ?: [dir stringByAppendingPathComponent:@"history.bin"];
    history_open(hp.fileSystemRepresentation);

    gItem = [NSStatusBar.systemStatusBar statusItemWithLength:60];
    gItem.autosaveName = @"StatsLite";
    NSStatusBarButton *button = gItem.button;
    gStrip = [[StripView alloc] initWithFrame:button.bounds];
    [button addSubview:gStrip];
    gContent = [CALayer layer];
    gContent.contentsGravity = kCAGravityBottomLeft;
    gContent.anchorPoint = CGPointZero;
    gContent.frame = gStrip.bounds;
    gContent.actions = @{@"contents" : NSNull.null, @"bounds" : NSNull.null, @"position" : NSNull.null};
    [gStrip.layer addSublayer:gContent];
    gTipOwner = self;
    refresh_env();
    button.target = self;
    button.action = @selector(clicked:);
    [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];

    NSNotificationCenter *wc = NSWorkspace.sharedWorkspace.notificationCenter;
    [wc addObserver:self selector:@selector(screensSlept:) name:NSWorkspaceScreensDidSleepNotification object:nil];
    [wc addObserver:self selector:@selector(screensWoke:) name:NSWorkspaceScreensDidWakeNotification object:nil];
    [wc addObserver:self selector:@selector(willSleep:) name:NSWorkspaceWillSleepNotification object:nil];
    [wc addObserver:self selector:@selector(didWake:) name:NSWorkspaceDidWakeNotification object:nil];
    [wc addObserver:self selector:@selector(sessionResigned:) name:NSWorkspaceSessionDidResignActiveNotification object:nil];
    [wc addObserver:self selector:@selector(sessionActivated:) name:NSWorkspaceSessionDidBecomeActiveNotification object:nil];
    NSDistributedNotificationCenter *dc = NSDistributedNotificationCenter.defaultCenter;
    [dc addObserver:self selector:@selector(locked:) name:@"com.apple.screenIsLocked" object:nil];
    [dc addObserver:self selector:@selector(unlocked:) name:@"com.apple.screenIsUnlocked" object:nil];
    S.thermal = (int)NSProcessInfo.processInfo.thermalState;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(thermalChanged:)
                                               name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(colorsChanged:)
                                               name:NSSystemColorsDidChangeNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(occlusionChanged:)
                                               name:NSWindowDidChangeOcclusionStateNotification object:button.window];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(itemMoved:)
                                               name:NSWindowDidMoveNotification object:button.window];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(itemMoved:)
                                               name:NSApplicationDidChangeScreenParametersNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(itemMoved:)
                                               name:NSWindowDidChangeScreenNotification object:button.window];

    apply_modules();
    schedule_timer(1);
#if LITE_DEV
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"selfTest"]) { [self selfTest]; return; }
#endif
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"benchPanel"])  // benchmark: measure the open-panel cost
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ panel_open(); });
}

- (void)applicationWillTerminate:(NSNotification *)n { history_close(); }

- (void)clicked:(id)sender {
    NSEvent *e = NSApp.currentEvent;
    if (e.type == NSEventTypeRightMouseUp || (e.modifierFlags & NSEventModifierFlagControl)) {
        LITE_LOG(@"close: right-click");
        panel_close();
        NSMenu *menu = [self buildMenu];
#if LITE_DEV
        gStMenu = menu;
#endif
        [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, gItem.button.bounds.size.height + 4) inView:gItem.button];
#if LITE_DEV
        gStMenu = nil;
#endif
        return;
    }
    LITE_LOG(@"click: panel %d event %@", S.panel_open, e);
    if (S.panel_open) panel_close(); else panel_open();
}

#if LITE_DEV
- (void)selfTest {
    gStDir = [NSUserDefaults.standardUserDefaults stringForKey:@"selfTestDir"] ?: NSTemporaryDirectory();
    // The test drives every setting, and settings are saved as they change: put the
    // user's saved settings back when it finishes.
    static NSDictionary *saved;
    saved = [NSUserDefaults.standardUserDefaults persistentDomainForName:NSBundle.mainBundle.bundleIdentifier];
    NSMutableArray<dispatch_block_t> *steps = [NSMutableArray array];
    __weak App *weakSelf = self;
    // 0. real mouse events through AppKit: left click opens, left click closes,
    //    right click shows the settings menu (verified while open, then dismissed)
    [steps addObject:^{ st_click(0); }];
    [steps addObject:^{ st_check(S.panel_open && gPanel.isVisible, @"mouse click on the status item opens the panel"); st_click(0); }];
    [steps addObject:^{ st_check(!S.panel_open && !gPanel.isVisible, @"second mouse click closes it"); st_click(0); }];
    [steps addObject:^{
        st_check(S.panel_open, @"panel open before right click");
        // the menu runs a modal tracking loop: check it from a timer in that loop
        CFRunLoopTimerRef tm = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 0, 0, 0, ^(CFRunLoopTimerRef x) {
            st_check(gStMenu != nil && gStMenu.numberOfItems >= M_COUNT + 5, @"right click shows the settings menu");
            st_check(!S.panel_open, @"right click closes the panel first");
            [gStMenu cancelTracking];
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), tm, kCFRunLoopCommonModes);
        CFRelease(tm);
        st_click(1);
        st_check(gStMenu == nil, @"menu dismissed");
    }];
    // 1. menu bar with defaults
    [steps addObject:^{ st_check(S.last_width > 20, @"menu bar strip has a width"); st_check(gContent.contents != nil, @"strip has contents"); st_dump(@"default"); }];
    [steps addObject:^{
        NSString *tip = [weakSelf view:gItem.button stringForToolTip:0 point:NSZeroPoint userData:NULL];
        printf("tooltip:\n%s\n", tip.UTF8String);
        st_check([tip containsString:@"CPU"] && [tip containsString:@"peak"] && [tip containsString:@"Disk"], @"hover tooltip lists current values and recent peaks");
    }];
    // 2. panel open (same path as a left click)
    [steps addObject:^{ [weakSelf clicked:nil]; }];
    [steps addObject:^{ st_check(S.panel_open && gPanel.isVisible, @"left click opens the panel"); st_check(gLeanView->_static.contents && gLeanView->_content.contents, @"panel layers have contents"); st_check(st_panel_placed(), @"panel sits under the status item, sized to its content"); st_dump(@"panel"); }];
    // 3. scroll the panel
    [steps addObject:^{ gPanelScroll = 200; panel_refresh(); st_check(gPanelScroll >= 0, @"panel scroll clamps"); st_dump(@"panel-scrolled"); }];
    // 4. close (same path as a second click)
    [steps addObject:^{ [weakSelf clicked:nil]; st_check(!S.panel_open && !gPanel.isVisible, @"second click closes the panel"); st_check(gLeanView->_content.contents == nil, @"closed panel releases its surfaces"); }];
    // 5. every module on, every style
    [steps addObject:^{
        for (int m = 0; m < M_COUNT; m++) if (!(S.modules & (1u << m))) [weakSelf toggleModule:st_item(m)];
        st_check(S.modules == (1u << M_COUNT) - 1, @"all modules enabled from the menu");
    }];
    [steps addObject:^{ st_dump(@"all-modules"); }];
    for (int k = 1; k <= 4; k++) {
        [steps addObject:^{
            for (int m = 0; m < M_COUNT; m++) [weakSelf setStyle:st_item(m * 8 + k)];
            if (k == STYLE_SPARK) {
                int sparks = 0;
                strip_t st;
                build_strip(&st);
                for (int i = 0; i < st.ncells; i++) sparks += st.cell[i].spark;
                st_check(sparks >= 3, [NSString stringWithFormat:@"sparkline style puts sparklines in the strip (%d cells)", sparks]);
            }
            st_check(S.style[M_CPU] == k && S.style[M_NET] == k, [NSString stringWithFormat:@"style %d applied to all modules", k]);
        }];
        [steps addObject:^{ st_check(S.last_width > 20, @"strip rebuilt after style change"); st_dump([NSString stringWithFormat:@"style-%d", k]); }];
    }
    // 6. monochrome, labels off/on
    [steps addObject:^{ [weakSelf toggleMono:nil]; [weakSelf toggleLabels:nil]; }];
    [steps addObject:^{ st_check(S.mono && !S.labels, @"monochrome on, labels off"); st_dump(@"mono-nolabels"); [weakSelf toggleMono:nil]; [weakSelf toggleLabels:nil]; }];
    // 7. appearance switches (as when the menu bar flips light/dark)
    [steps addObject:^{ gStrip.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua]; }];
    [steps addObject:^{ st_check(S.env_dark == 0, @"light appearance picked up"); st_dump(@"light"); gStrip.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; }];
    [steps addObject:^{ st_check(S.env_dark == 1, @"dark appearance picked up"); st_dump(@"dark"); }];
    // 8. panel with everything, then interval change while open
    [steps addObject:^{ [weakSelf clicked:nil]; }];
    [steps addObject:^{ st_check(S.panel_open, @"panel reopens with all modules"); st_check(st_panel_placed(), @"taller panel repositioned and resized"); st_dump(@"panel-all"); [weakSelf setEvery:st_item(M_GPU * 256 + 3)]; }];
    [steps addObject:^{
        st_check(module_period(M_GPU) == 3 && base_period() == 1, @"per-module refresh applies with the panel open (GPU every 3 s, timer still 1 s)");
        st_dump(@"panel-gpu3");
        [weakSelf clicked:nil];
        [weakSelf setEvery:st_item(M_GPU * 256 + 2)];
    }];
    [steps addObject:^{  // every visible metric at 2 s or slower: the timer itself slows to 2 s
        unsigned keep = S.modules;
        S.modules = (1u << M_CPU) | (1u << M_GPU) | (1u << M_MEM) | (1u << M_DISK);
        int c = gEvery[M_CPU], mm = gEvery[M_MEM];
        gEvery[M_CPU] = 2; gEvery[M_MEM] = 4;
        st_check(!S.panel_open && base_period() == 2, [NSString stringWithFormat:@"timer follows the gcd of the refresh rates (%d s)", base_period()]);
        gEvery[M_CPU] = (uint8_t)c; gEvery[M_MEM] = (uint8_t)mm;
        st_check(base_period() == 1, @"back to 1 s with CPU every second");
        S.modules = keep;
    }];
    // 9. settings menu builds and reflects state
    [steps addObject:^{
        NSMenu *m = [weakSelf buildMenu];
        st_check(m.numberOfItems >= M_COUNT + 5, @"settings menu builds");
        st_check([m itemWithTitle:@"CPU"].state == NSControlStateValueOn, @"menu shows CPU enabled");
    }];
    // 10. modules off again except CPU
    [steps addObject:^{
        for (int m = M_COUNT - 1; m > 0; m--) if (S.modules & (1u << m)) [weakSelf toggleModule:st_item(m)];
        [weakSelf toggleModule:st_item(M_CPU)];  // must refuse to remove the last module
        st_check(S.modules == (1u << M_CPU), @"last module cannot be removed");
    }];
    [steps addObject:^{
        st_dump(@"cpu-only");
        NSString *bid = NSBundle.mainBundle.bundleIdentifier;
        if (saved) [NSUserDefaults.standardUserDefaults setPersistentDomain:saved forName:bid];
        else [NSUserDefaults.standardUserDefaults removePersistentDomainForName:bid];
        [NSUserDefaults.standardUserDefaults synchronize];
        NSDictionary *now = [NSUserDefaults.standardUserDefaults persistentDomainForName:bid];
        st_check(saved ? [now isEqualToDictionary:saved] : now.count == 0, @"saved settings restored after the test");
        printf("SELFTEST %s (%d failures)\n", gStFail ? "FAILED" : "PASSED", gStFail);
        exit(gStFail ? 1 : 0);
    }];
    __block void (^run)(NSUInteger) = nil;
    __block __weak void (^weakRun)(NSUInteger);
    weakRun = run = ^(NSUInteger i) {
        if (i >= steps.count) return;
        steps[i]();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ weakRun(i + 1); });
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ run(0); });
    objc_setAssociatedObject(self, "run", run, OBJC_ASSOCIATION_COPY);
}
#endif

// Refresh choices per module (seconds; 0 ends the list). Battery is event-driven,
// the clock follows the minute.
#define EVERY_DISKIO M_COUNT
static const uint8_t kEveryOptions[M_COUNT + 1][8] = {
    {1, 2, 3, 5, 10, 0},       // CPU
    {1, 2, 3, 5, 10, 0},       // GPU
    {1, 2, 3, 5, 10, 0},       // memory
    {10, 30, 60, 120, 240, 0}, // disk free space
    {1, 2, 3, 5, 10, 0},       // network
    {1, 2, 3, 5, 10, 0},       // sensors
    {0},                       // battery: event-driven
    {30, 60, 120, 240, 0},     // Bluetooth accessories
    {0},                       // clock: on the minute
    {1, 2, 3, 5, 0},           // disk read/write rates
};
static void add_every_menu(NSMenu *into, NSString *title, int m, id target) {
    NSMenuItem *head = [into addItemWithTitle:title action:nil keyEquivalent:@""];
    NSMenu *sub = [[NSMenu alloc] initWithTitle:@""];
    int cur = m == EVERY_DISKIO ? gEveryDiskIO : (gEvery[m] ? gEvery[m] : kEveryDefault[m]);
    int def = m == EVERY_DISKIO ? 1 : kEveryDefault[m];
    for (int k = 0; k < 8 && kEveryOptions[m][k]; k++) {
        int sec = kEveryOptions[m][k];
        NSString *t = sec >= 60 && sec % 60 == 0 ? [NSString stringWithFormat:@"%d min", sec / 60] : [NSString stringWithFormat:@"%d s", sec];
        if (sec == def) t = [t stringByAppendingString:@" (default)"];
        NSMenuItem *it = [sub addItemWithTitle:t action:@selector(setEvery:) keyEquivalent:@""];
        it.tag = m * 256 + sec;
        it.target = target;
        it.state = sec == cur ? NSControlStateValueOn : NSControlStateValueOff;
    }
    head.submenu = sub;
}

- (NSMenu *)buildMenu {
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Stats Lite"];
    for (int i = 0; i < M_COUNT; i++) {
        NSMenuItem *it = [m addItemWithTitle:kModuleTitle[i] action:nil keyEquivalent:@""];
        it.state = (S.modules & (1u << i)) ? NSControlStateValueOn : NSControlStateValueOff;
        NSMenu *sub = [[NSMenu alloc] initWithTitle:@""];
        NSMenuItem *show = [sub addItemWithTitle:@"Show in menu bar" action:@selector(toggleModule:) keyEquivalent:@""];
        show.tag = i;
        show.target = self;
        show.state = it.state;
        if (kEveryOptions[i][0]) {  // refresh rate
            [sub addItem:NSMenuItem.separatorItem];
            add_every_menu(sub, i == M_DISK ? @"Free space every" : @"Refresh every", i, self);
            if (i == M_DISK) add_every_menu(sub, @"Read/write rates every", EVERY_DISKIO, self);
        }
        if (i != M_DISK && i != M_BATTERY) {
            [sub addItem:NSMenuItem.separatorItem];
            NSString *titles[5] = {nil, i == M_NET ? @"Speed" : @"Percentage", @"Chart", i == M_NET ? @"Speed and chart" : @"Percentage and chart",
                                   i == M_SENSORS ? @"Temperature and sparkline" : @"Percentage and sparkline"};
            int nstyles = i == M_NET ? 4 : 5;  // the speed cell has no label row for a sparkline
            for (int k = 1; k < nstyles; k++) {
                NSMenuItem *st = [sub addItemWithTitle:titles[k] action:@selector(setStyle:) keyEquivalent:@""];
                st.tag = i * 8 + k;
                st.target = self;
                st.state = (S.style[i] ? S.style[i] : 1) == k ? NSControlStateValueOn : NSControlStateValueOff;
            }
        }
        it.submenu = sub;
    }
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *al = [m addItemWithTitle:@"Notifications" action:nil keyEquivalent:@""];
    NSMenu *asub = [[NSMenu alloc] initWithTitle:@""];
    for (int a = 0; a < A_COUNT; a++) {
        NSMenuItem *it = [asub addItemWithTitle:kAlertTitle[a] action:@selector(toggleAlert:) keyEquivalent:@""];
        it.tag = a;
        it.target = self;
        it.state = gAlerts.enabled[a] ? NSControlStateValueOn : NSControlStateValueOff;
    }
    al.submenu = asub;
    NSMenuItem *col = [m addItemWithTitle:@"Colored charts" action:@selector(toggleMono:) keyEquivalent:@""];
    col.target = self;
    col.state = S.mono ? NSControlStateValueOff : NSControlStateValueOn;
    NSMenuItem *lab = [m addItemWithTitle:@"Show labels" action:@selector(toggleLabels:) keyEquivalent:@""];
    lab.target = self;
    lab.state = S.labels ? NSControlStateValueOn : NSControlStateValueOff;
    if (@available(macOS 13.0, *)) {
        NSMenuItem *login = [m addItemWithTitle:@"Launch at login" action:@selector(toggleLogin:) keyEquivalent:@""];
        login.target = self;
        login.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Quit Stats Lite" action:@selector(terminate:) keyEquivalent:@"q"];
    return m;
}

- (void)toggleModule:(NSMenuItem *)it {
    unsigned bit = 1u << it.tag;
    if ((S.modules & bit) && (S.modules & ~bit) == 0) return;  // keep at least one
    S.modules ^= bit;
    save_prefs();
    apply_modules();
    schedule_timer(1);
}
- (void)setStyle:(NSMenuItem *)it {
    S.style[it.tag / 8] = (uint8_t)(it.tag % 8);
    save_prefs();
    request_redraw();
}
// tag = module * 256 + seconds (module EVERY_DISKIO: disk read/write rates)
- (void)setEvery:(NSMenuItem *)it {
    int m = (int)(it.tag / 256), sec = (int)(it.tag % 256);
    if (m == EVERY_DISKIO) gEveryDiskIO = (uint8_t)sec;
    else if (m >= 0 && m < M_COUNT) gEvery[m] = (uint8_t)sec;
    save_prefs();
    schedule_timer(1);
}
- (void)toggleAlert:(NSMenuItem *)it {
    int a = (int)it.tag;
    gAlerts.enabled[a] = !gAlerts.enabled[a];
    gAlerts.since[a] = 0;
    [NSUserDefaults.standardUserDefaults setBool:gAlerts.enabled[a] forKey:kAlertKey[a]];
    if (gAlerts.enabled[a])  // ask once, only when the user turns an alert on
        [UNUserNotificationCenter.currentNotificationCenter requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                                                                          completionHandler:^(BOOL granted, NSError *e) {}];
    apply_modules();
}
- (void)toggleMono:(NSMenuItem *)it {
    S.mono = !S.mono;
    save_prefs();
    request_redraw();
}
- (void)toggleLabels:(NSMenuItem *)it {
    S.labels = !S.labels;
    save_prefs();
    request_redraw();
}
- (void)toggleLogin:(NSMenuItem *)it API_AVAILABLE(macos(13.0)) {
    NSError *err = nil;
    if (SMAppService.mainAppService.status == SMAppServiceStatusEnabled) [SMAppService.mainAppService unregisterAndReturnError:&err];
    else [SMAppService.mainAppService registerAndReturnError:&err];
    if (err) NSLog(@"launch at login: %@", err);
}

- (void)colorsChanged:(NSNotification *)n { refresh_env(); request_redraw(); }
- (void)itemMoved:(NSNotification *)n { gPanelPosDirty = 1; if (S.panel_open) panel_position(); }
// Asked only while the pointer rests on the item; nothing is computed otherwise.
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void *)data {
    return S.panel_open ? nil : tooltip_text();
}
- (void)thermalChanged:(NSNotification *)n {
    // Posted on an arbitrary thread.
    dispatch_async(dispatch_get_main_queue(), ^{
        S.thermal = (int)NSProcessInfo.processInfo.thermalState;
        if (!suspended()) request_redraw();
    });
}
- (void)screensSlept:(NSNotification *)n { S.screens_asleep = 1; suspend_changed(); }
- (void)screensWoke:(NSNotification *)n { S.screens_asleep = 0; suspend_changed(); }
- (void)willSleep:(NSNotification *)n { S.system_asleep = 1; suspend_changed(); }
- (void)didWake:(NSNotification *)n { S.system_asleep = 0; sampler_net_invalidate(); suspend_changed(); }
- (void)sessionResigned:(NSNotification *)n { LITE_LOG(@"close: session resigned"); S.inactive = 1; panel_close(); suspend_changed(); }
- (void)sessionActivated:(NSNotification *)n { S.inactive = 0; suspend_changed(); }
- (void)locked:(NSNotification *)n { LITE_LOG(@"close: locked"); S.locked = 1; panel_close(); suspend_changed(); }
- (void)unlocked:(NSNotification *)n { S.locked = 0; suspend_changed(); }
- (void)occlusionChanged:(NSNotification *)n {
    int occ = !(gItem.button.window.occlusionState & NSWindowOcclusionStateVisible);
    if (occ == S.occluded) return;
    S.occluded = occ;
    suspend_changed();
}
@end

// ---- Entry -----------------------------------------------------------------------
static int cli(int argc, const char **argv) {
    // --render out.png [dark|light] [modules]: render the live strip to a PNG.
    if (argc >= 3 && !strcmp(argv[1], "--render")) {
        int dark = argc < 4 || strcmp(argv[3], "light") != 0;
        renderer_init(&S.renderer);
        load_prefs();
        if (argc >= 5) {
            S.modules = 0;
            for (NSString *k in [@(argv[4]) componentsSeparatedByString:@","])
                for (int m = 0; m < M_COUNT; m++) if ([k isEqualToString:kModuleKey[m]]) S.modules |= 1u << m;
        }
        sampler_init();
        sampler_refresh_purgeable(0);
        NSColor *acc = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        S.env_accent[0] = (float)acc.redComponent; S.env_accent[1] = (float)acc.greenComponent; S.env_accent[2] = (float)acc.blueComponent;
        for (int i = 0; i < 40; i++) {  // fill the chart rings with real samples
            usleep(100000);
            S.last_tick = i ? S.last_tick : 0;
            tick_sample_only();
        }
        sampler_battery(&S.snap);
        strip_t st;
        build_strip_env(&st, dark, 2, 24);
        CGFloat w = renderer_measure(&S.renderer, &st);
        CGImageRef img = renderer_draw(&S.renderer, &st, w);
        // Composite over a menu-bar-like background at 2x, scaled up 4x for inspection.
        int pw = (int)CGImageGetWidth(img), ph = (int)CGImageGetHeight(img), z = 3;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef c = CGBitmapContextCreate(NULL, pw * z, ph * z, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextSetRGBFillColor(c, dark ? 0.12 : 0.93, dark ? 0.12 : 0.93, dark ? 0.13 : 0.94, 1);
        CGContextFillRect(c, CGRectMake(0, 0, pw * z, ph * z));
        CGContextSetInterpolationQuality(c, kCGInterpolationNone);
        CGContextDrawImage(c, CGRectMake(0, 0, pw * z, ph * z), img);
        CGImageRef out = CGBitmapContextCreateImage(c);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:out];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
        printf("rendered %d x %d px (%.0f pt wide)\n", pw, ph, w);
        return 0;
    }
    // --sample N: print N snapshots (1 s apart) for verification without UI.
    if (argc >= 2 && !strcmp(argv[1], "--sample")) {
        int n = argc >= 3 ? atoi(argv[2]) : 3;
        sampler_init();
        snapshot_t s;
        memset(&s, 0, sizeof s);
        unsigned need = NEED_CPU | NEED_CPU_CORES | NEED_LOAD | NEED_MEM | NEED_MEM_DETAIL | NEED_GPU | NEED_DISK_SPACE |
                        NEED_DISK_IO | NEED_NET | NEED_SENSORS;
        sampler_refresh_purgeable(0);
        for (int i = 0; i < n; i++) {
            sleep(1);
            sampler_collect(need, 1.0, &s);
            sampler_battery(&s);
            printf("cpu %.1f%% (u %.1f s %.1f) | gpu %d %.0f%% | mem %.2f/%.2f GB p%d swap %.2f GB | disk free %.1f/%.1f GB "
                   "purge %.1f r %.0f w %.0f B/s | net %s in %.0f out %.0f B/s | temp cpu %.1f gpu %.1f fans %d %.0f power %.1f W | "
                   "batt %d %.0f%% chg %d ac %d %d min\n",
                   s.cpu_total * 100, s.cpu_user * 100, s.cpu_system * 100, s.gpu_ok, s.gpu_util * 100, s.mem_used / 1e9,
                   s.mem_total / 1e9, s.mem_pressure, s.swap_used / 1e9, s.disk_free / 1e9, s.disk_total / 1e9,
                   s.disk_purgeable / 1e9, s.disk_read_bps, s.disk_write_bps, s.net_if, s.net_in_bps, s.net_out_bps,
                   s.temp_cpu, s.temp_gpu, s.nfans, s.fan_rpm[0], s.power_system, s.batt_present, s.batt_level * 100,
                   s.batt_charging, s.batt_ac, s.batt_minutes);
            for (int k = 0; k < s.nclusters; k++) printf("   %s %.0f%%", s.cluster_name[k], s.cluster_avg[k] * 100);
            printf("   cores %d\n", s.ncores);
        }
        proc_row_t c[5], m[5], g[5];
        int nc, nm, ng;
        procs_sample(1, c, &nc, m, &nm, 5);
        gpuprocs_sample(-1, g, &ng, 5);
        sleep(1);
        procs_sample(1, c, &nc, m, &nm, 5);
        gpuprocs_sample(-1, g, &ng, 5);
        for (int i = 0; i < nc; i++) printf("top cpu %-20s %5.1f%%   top mem %-20s %6.0f MB\n", c[i].name, c[i].cpu * 100, m[i].name, m[i].memory / 1e6);
        for (int i = 0; i < ng; i++) printf("top gpu %-20s %5.1f%%\n", g[i].name, g[i].cpu * 100);
        return 0;
    }
    return -1;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        int r = cli(argc, argv);
        if (r >= 0) return r;
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyAccessory;
        App *d = [App new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
