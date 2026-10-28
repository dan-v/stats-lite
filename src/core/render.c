#include "render.h"

#include <IOSurface/IOSurface.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Geometry mirrors the original Stats "mini" widget (Kit/Widgets/Mini.swift):
// 31 pt wide with label, label 7 pt light at y=12, value 12 pt at y=1, 2 pt gaps.
#define MINI_W 31.0
#define SPEED_W 50.0
#define BATT_W 52.0  // body 22 + nub + gap + "100+" at 9 pt, so ink never leaves the cell
#define GAP 4.0
#define PAD 2.0

void glyphset_init(glyphset_t *g, CTFontRef font) {
    memset(g, 0, sizeof(*g));
    g->font = (CTFontRef)CFRetain(font);
    // Resolve each character through CoreText shaping once, so font features such
    // as tabular (monospaced) digits are applied; a plain cmap lookup ignores them.
    CFStringRef keys[] = {kCTFontAttributeName, kCTLigatureAttributeName};
    int zero = 0;
    CFNumberRef nolig = CFNumberCreate(NULL, kCFNumberIntType, &zero);
    CFTypeRef vals[] = {font, nolig};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 2,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < 128; i++) {
        UniChar ch = (UniChar)(i < 32 ? ' ' : i);
        if (i == CH_DEGREE) ch = 0x00B0;
        if (i == CH_UP) ch = 0x2191;
        if (i == CH_DOWN) ch = 0x2193;
        CFStringRef str = CFStringCreateWithCharacters(NULL, &ch, 1);
        CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        if (CFArrayGetCount(runs) > 0) {
            CTRunRef run = CFArrayGetValueAtIndex(runs, 0);
            if (CTRunGetGlyphCount(run) > 0) {
                CTRunGetGlyphs(run, CFRangeMake(0, 1), &g->glyph[i]);
                CGSize a;
                CTRunGetAdvances(run, CFRangeMake(0, 1), &a);
                g->adv[i] = a.width;
            }
        }
        CFRelease(line); CFRelease(as); CFRelease(str);
    }
    CFRelease(attrs); CFRelease(nolig);
    g->ascent = CTFontGetAscent(font);
    g->descent = CTFontGetDescent(font);
}

CGFloat glyphset_width(const glyphset_t *g, const char *s) {
    CGFloat w = 0;
    for (; *s; s++) w += g->adv[(unsigned char)*s & 127];
    return w;
}

void glyphset_draw(CGContextRef ctx, const glyphset_t *g, const char *s, CGFloat x, CGFloat y) {
    CGGlyph gl[32];
    CGPoint pos[32];
    size_t n = 0;
    for (; *s && n < 32; s++, n++) {
        unsigned c = (unsigned char)*s & 127;
        gl[n] = g->glyph[c];
        pos[n] = CGPointMake(x, y);
        x += g->adv[c];
    }
    CTFontDrawGlyphs(g->font, gl, pos, n, ctx);
}

static CTFontRef system_font(CGFloat size, CGFloat weight, int monodigits) {
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberCGFloatType, &weight);
    const void *tk[] = {kCTFontWeightTrait};
    const void *tv[] = {w};
    CFDictionaryRef traits = CFDictionaryCreate(NULL, tk, tv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);
    if (monodigits) {  // kNumberSpacingType / kMonospacedNumbersSelector
        int t = 6, sel = 0;
        CFNumberRef nt = CFNumberCreate(NULL, kCFNumberIntType, &t), ns = CFNumberCreate(NULL, kCFNumberIntType, &sel);
        const void *fk[] = {kCTFontFeatureTypeIdentifierKey, kCTFontFeatureSelectorIdentifierKey};
        const void *fv[] = {nt, ns};
        CFDictionaryRef feat = CFDictionaryCreate(NULL, fk, fv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayRef feats = CFArrayCreate(NULL, (const void **)&feat, 1, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, feats);
        CFRelease(feats); CFRelease(feat); CFRelease(nt); CFRelease(ns);
    }
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CTFontRef f = CTFontCreateCopyWithAttributes(base, size, NULL, desc);
    CFRelease(desc); CFRelease(attrs); CFRelease(traits); CFRelease(w); CFRelease(base);
    return f;
}

void renderer_init(renderer_t *r) {
    memset(r, 0, sizeof(*r));
    // Weights: light ~ -0.4 (NSFontWeightLight), regular 0.
    CTFontRef lf = system_font(7, -0.4, 0), vf = system_font(12, 0, 1), sf = system_font(9, 0, 1);
    glyphset_init(&r->label, lf);
    glyphset_init(&r->value, vf);
    glyphset_init(&r->small, sf);
    colorlut_init(&r->lut_dark, 1, 1, 1, 1);
    colorlut_init(&r->lut_light, 0, 0, 0, 0.85f);
    colorlut_init(&r->lut_warn, 1.0f, 0.27f, 0.23f, 1);
    CFRelease(lf); CFRelease(vf); CFRelease(sf);
}

#define CHART_BOX_H 18.0  // points, the original's widget box height

int renderer_chart_cols(float scale) {
    int n = (int)lroundf((CHART_W - 2) * scale);  // 1 pt inset each side
    return n > CHART_COLS ? CHART_COLS : n;
}
int renderer_chart_height(float scale, int half) {
    int h = (int)lroundf((CHART_BOX_H - 2) * scale);  // inner height
    return half ? h / 2 : h;
}

static CGFloat cell_width(const cell_t *c, int labels) {
    if (c->wide) return c->wide;
    switch (c->kind) {
    case CELL_CHART: case CELL_NETCHART: return CHART_W;
    case CELL_SPEED: return SPEED_W;
    case CELL_BATTERY: return BATT_W;
    default: return labels ? MINI_W : MINI_W + 5;
    }
}

CGFloat renderer_measure(const renderer_t *r, const strip_t *s) {
    (void)r;
    CGFloat w = PAD * 2;
    for (int i = 0; i < s->ncells; i++) w += cell_width(&s->cell[i], s->labels) + (i ? GAP : 0);
    return ceil(w);
}

static void ensure_ctx(renderer_t *r, int pw, int ph, float scale, int exact) {
    if (r->ctx && (exact ? r->pw == pw : r->pw >= pw) && r->ph == ph && r->scale == scale) return;
    if (r->ctx) { CGContextRelease(r->ctx); free(r->pixels); }
    int alloc_w = exact ? pw : pw + 64;  // slack so small width changes don't reallocate
    r->pixels = calloc((size_t)alloc_w * ph, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    r->ctx = CGBitmapContextCreate(r->pixels, (size_t)alloc_w, (size_t)ph, 8, (size_t)alloc_w * 4, cs,
                                   kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    r->pw = alloc_w;
    r->ph = ph;
    r->scale = scale;
    CGContextScaleCTM(r->ctx, scale, scale);
    CGContextSetTextMatrix(r->ctx, CGAffineTransformIdentity);
    CGContextSetShouldSubpixelPositionFonts(r->ctx, true);
    CGContextSetShouldSubpixelQuantizeFonts(r->ctx, true);
}

static void draw_battery(CGContextRef ctx, const renderer_t *r, const cell_t *c, CGFloat x, CGFloat mid, CGColorRef fg) {
    // body 22x10 + nub, fill proportional to level, percentage on the right
    CGFloat bw = 22, bh = 10, y = mid - bh / 2;
    CGRect body = CGRectMake(x + 0.5, y + 0.5, bw - 1, bh - 1);
    CGContextSetStrokeColorWithColor(ctx, fg);
    CGContextSetLineWidth(ctx, 1);
    CGPathRef p = CGPathCreateWithRoundedRect(body, 2.5, 2.5, NULL);
    CGContextAddPath(ctx, p);
    CGContextStrokePath(ctx);
    CGPathRelease(p);
    CGContextSetFillColorWithColor(ctx, fg);
    CGContextFillRect(ctx, CGRectMake(x + bw, mid - 2, 1.5, 4));
    CGFloat fw = (bw - 4) * c->level / 100.0;
    if (fw > 0) CGContextFillRect(ctx, CGRectMake(x + 2, y + 2, fw, bh - 4));
    char pct[8];
    int lv = c->level, n = 0;
    if (lv >= 100) pct[n++] = '1';
    if (lv >= 10) pct[n++] = (char)('0' + (lv / 10) % 10);
    pct[n++] = (char)('0' + lv % 10);
    if (c->charging) pct[n++] = '+';
    pct[n] = 0;
    glyphset_draw(ctx, &r->small, pct, x + bw + 3, mid - 3.2);
}

static int blitting(const renderer_t *r) { return r->mode == RENDER_BLIT_IMAGE || r->mode == RENDER_BLIT_SURFACE; }

// Text through either CoreGraphics (t == NULL) or the atlas blitter.
static void text(renderer_t *r, CGContextRef ctx, const target_t *t, glyphset_t *g, const char *str, CGFloat x, CGFloat y,
                 const colorlut_t *lut) {
    if (!t) { glyphset_draw(ctx, g, str, x, y); return; }
    // Light-on-dark menu bars use white ink; dark-on-light uses black (or the warn color: close enough to light ink).
    int dark_ink = !r->dark_for_atlas && lut != &r->lut_warn;
    atlas_t *a = &g->atlas[dark_ink];
    if (a->scale != r->scale_for_atlas || !a->pix) atlas_build(a, g->font, g->glyph, r->scale_for_atlas, dark_ink);
    BLIT_TEXT(t, a, str, (float)(x * a->scale), (int)lround(y * a->scale), lut);
}

// Device rows [*lo, *hi) (from the bottom) that str's glyph masks cover when drawn
// with its baseline at y points (blit modes).
static void ink_rows(renderer_t *r, glyphset_t *g, const char *str, CGFloat y, int *lo, int *hi) {
    atlas_t *a = &g->atlas[0];
    if (a->scale != r->scale_for_atlas || !a->pix) atlas_build(a, g->font, g->glyph, r->scale_for_atlas, 0);
    int base = (int)lround(y * a->scale);
    *lo = INT32_MAX;
    *hi = INT32_MIN;
    for (; *str; str++) {
        const gmask_t *m = &a->m[(unsigned char)*str & 127];
        if (!m->w) continue;
        if (base + m->y0 < *lo) *lo = base + m->y0;
        if (base + m->y0 + m->h > *hi) *hi = base + m->y0 + m->h;
    }
}

// Chart box: subtle rounded background, then columns. Pure pixel loops.
typedef struct {
    int bx, by, bw, bh, x0, iy, ih, n;  // box, first column, inner rows, columns in use
    uint32_t bg, fill, line, band, ufill, uline;
} chart_geo_t;

static void chart_geo(chart_geo_t *g, const cell_t *c, const strip_t *s, CGFloat x, CGFloat base) {
    float sc = s->scale;
    g->bx = (int)lroundf((float)x * sc);
    g->by = (int)lroundf((float)base * sc);
    g->bw = (int)lroundf(CHART_W * sc);
    g->bh = (int)lroundf((float)CHART_BOX_H * sc);
    float fr = s->dark ? 1 : 0;
    g->bg = premul_rgba(fr, fr, fr, s->dark ? 0.13f : 0.10f);
    int inset = (int)lroundf(sc);
    g->iy = g->by + inset;
    g->ih = g->bh - 2 * inset;
    g->n = c->ncols;
    g->x0 = g->bx + inset + (g->bw - 2 * inset) - g->n;  // right-align: newest at the right edge
    float cr = s->mono ? fr : s->accent[0], cg = s->mono ? fr : s->accent[1], cb = s->mono ? fr : s->accent[2];
    g->fill = premul_rgba(cr, cg, cb, s->mono ? 0.38f : 0.45f);
    g->line = premul_rgba(cr, cg, cb, s->mono ? 0.95f : 1);
    g->band = premul_rgba(cr, cg, cb, s->mono ? 0.42f : 0.5f);
    g->ufill = s->mono ? g->fill : premul_rgba(1.0f, 0.42f, 0.36f, 0.45f);
    g->uline = s->mono ? g->line : premul_rgba(1.0f, 0.42f, 0.36f, 1);
}

// Sparkline in a mini cell's label row: right of the label, bottom on the label's
// baseline, no box (background is the empty strip).
#define SPARK_X 16.0f
#define SPARK_W 15.0f
#define SPARK_H 6.0f
int renderer_spark_cols(float scale) {
    int n = (int)lroundf(SPARK_W * scale);
    return n > CHART_COLS ? CHART_COLS : n;
}
int renderer_spark_height(float scale) { return (int)lroundf(SPARK_H * scale); }

static void spark_geo(chart_geo_t *g, const cell_t *c, const strip_t *s, CGFloat x, CGFloat base) {
    chart_geo(g, c, s, x, base);  // colors
    float sc = s->scale;
    g->bx = (int)lroundf(((float)x + SPARK_X) * sc);
    g->bw = renderer_spark_cols(sc);
    g->by = g->iy = (int)lroundf(((float)base + 12) * sc);
    g->bh = g->ih = renderer_spark_height(sc);
    g->bg = 0;
    g->x0 = g->bx + g->bw - g->n;
}

// The chart's series for columns [i0, i1), over the box background.
static void chart_series(const target_t *px, const cell_t *c, const chart_geo_t *g, int i0, int i1) {
    if (c->kind != CELL_NETCHART) {  // column chart, or a mini cell's sparkline
        blit_columns_range(px, g->x0, g->iy, g->ih, c->col, i0, i1, g->fill, g->line, 0);
        if (c->stacked)  // lower band, drawn over the area in a denser shade
            blit_columns_range(px, g->x0, g->iy, g->ih, c->col2, i0, i1, g->band, g->band, 0);
    } else {
        // Network: download grows up from the middle, upload grows down; shared scale.
        int half = g->ih / 2;
        blit_columns_range(px, g->x0, g->iy + g->ih - half, half, c->col2, i0, i1, g->fill, g->line, 0);
        blit_columns_range(px, g->x0, g->iy, half, c->col, i0, i1, g->ufill, g->uline, 1);
    }
}

static void draw_chart(const target_t *px, const cell_t *c, const strip_t *s, CGFloat x, CGFloat base) {
    chart_geo_t g;
    chart_geo(&g, c, s, x, base);
    blit_fill(px, g.bx + 1, g.by, g.bw - 2, g.bh, g.bg);  // body with 1 px corner cuts
    blit_fill(px, g.bx, g.by + 1, 1, g.bh - 2, g.bg);
    blit_fill(px, g.bx + g.bw - 1, g.by + 1, 1, g.bh - 2, g.bg);
    chart_series(px, c, &g, 0, g.n);
}

// How the chart moved since `was` (this cell as the surface last showed it): k >= 0
// when the columns are the old ones moved k left (oldest dropped once full), except
// that the last kept column may have changed. -1: draw from scratch.
#define CHART_MAX_SHIFT 4
// *redo is the first column to redraw from: n - k when only the newest k columns
// are new, n - k - 1 when the last kept column changed too.
static int chart_shift(const cell_t *was, const cell_t *c, int *redo) {
    int n = c->ncols, no = was->ncols;
    if (was->kind != c->kind || was->stacked != c->stacked || was->spark != c->spark || n <= 2 * CHART_MAX_SHIFT || no > n) return -1;
    for (int k = 0; k <= CHART_MAX_SHIFT; k++) {
        int d = no + k - n;  // oldest columns dropped
        if (d < 0) continue;
        size_t same = (size_t)(n - k - 1);
        if (memcmp(was->col + d, c->col, same) != 0 || memcmp(was->col2 + d, c->col2, same) != 0) continue;
        int last_same = was->col[d + n - k - 1] == c->col[n - k - 1] && was->col2[d + n - k - 1] == c->col2[n - k - 1];
        if (k == 0 && last_same) continue;  // nothing moved or changed in the line
        *redo = last_same ? n - k : n - k - 1;
        return k;
    }
    return -1;
}

// Moves the old columns left by k and redraws the last kept column, the newest k,
// and the first column when the old first one was dropped (it has no connector).
static void scroll_chart(const target_t *px, const cell_t *was, const cell_t *c, const chart_geo_t *g, int k, int keep) {
    int n = g->n, d = was->ncols + k - n;
    for (int y = g->iy; y < g->iy + g->ih; y++) {
        int row = px->ph - 1 - y;
        if (row < 0 || row >= px->ph) continue;
        uint32_t *p = px->buf + (size_t)row * px->stride + g->x0;  // new first column
        if (k) memmove(p, p + k, (size_t)(n - k) * 4);              // old column d sat k to the right
        if (d) p[0] = g->bg;
        for (int i = keep; i < n; i++) p[i] = g->bg;
    }
    if (d) chart_series(px, c, g, 0, 1);
    chart_series(px, c, g, keep, n);
}

static int mini_text_same(const cell_t *a, const cell_t *b) {
    return a->warn == b->warn && a->wide == b->wide && memcmp(a->label, b->label, sizeof a->label) == 0 &&
           memcmp(a->top, b->top, sizeof a->top) == 0;
}

// dirty == NULL: repaint everything; else repaint only cells with dirty[i] != 0
// (their column range is cleared first; blit modes only).
// was: what the target showed before (partial repaints), so charts can scroll.
static void paint(renderer_t *r, CGContextRef ctx, const target_t *t, const strip_t *s, CGFloat clear_w, const uint8_t *dirty,
                  const strip_t *was) {
    if (!dirty) {
        if (t) blit_clear(t);
        else CGContextClearRect(ctx, CGRectMake(0, 0, clear_w, s->height + 1));
    }
    const target_t *px = t ? t : r->cur_target;  // pixel target for shapes (always available)
    r->scale_for_atlas = s->scale;
    r->dark_for_atlas = s->dark;
    const colorlut_t *lfg = s->dark ? &r->lut_dark : &r->lut_light;

    if (!r->cg_fg[0]) {
        r->cg_fg[0] = CGColorCreateSRGB(0, 0, 0, 0.85);
        r->cg_fg[1] = CGColorCreateSRGB(1, 1, 1, 1);
        r->cg_warn = CGColorCreateSRGB(1.0, 0.27, 0.23, 1);
    }
    CGColorRef fg = r->cg_fg[s->dark ? 1 : 0], warn = r->cg_warn;
    // Fill colors only matter to the CoreGraphics text path (t == NULL).
#define SET_FILL(col) do { if (!t) CGContextSetFillColorWithColor(ctx, col); } while (0)
    SET_FILL(fg);

    // Vertical frame: the original lays widgets out in a 22 pt box, inset 2 pt.
    CGFloat base = floor((s->height - 22) / 2) + 2;  // bottom of the 18 pt widget box
    CGFloat x = PAD;
    for (int i = 0; i < s->ncells; i++) {
        const cell_t *c = &s->cell[i];
        CGFloat w = cell_width(c, s->labels);
        if (dirty && !dirty[i]) { x += w + GAP; continue; }
        int chart = c->kind == CELL_CHART || c->kind == CELL_NETCHART;
        int spark = c->kind == CELL_MINI && c->spark && s->labels;
        int k = -1, keep = 0;
        if (dirty && was && px && (chart || (spark && mini_text_same(&was->cell[i], c)))) k = chart_shift(&was->cell[i], c, &keep);
        if (k >= 0) {
            chart_geo_t g;
            if (chart) chart_geo(&g, c, s, x, base);
            else spark_geo(&g, c, s, x, base);
            scroll_chart(px, &was->cell[i], c, &g, k, keep);
            r->charts_scrolled++;
            x += w + GAP;
            continue;
        }
        // Unchanged label: repaint only the value rows and/or the sparkline when their ink cannot overlap.
        if (dirty && was && t && c->kind == CELL_MINI && s->labels && was->cell[i].kind == CELL_MINI &&
            was->cell[i].wide == c->wide && was->cell[i].spark == c->spark && memcmp(was->cell[i].label, c->label, sizeof c->label) == 0) {
            const cell_t *o = &was->cell[i];
            int vchg = o->warn != c->warn || memcmp(o->top, c->top, sizeof c->top) != 0;
            int schg = spark && (o->ncols != c->ncols || memcmp(o->col, c->col, c->ncols) != 0);
            int vlo, vhi, llo, lhi, olo, ohi;
            ink_rows(r, &r->value, c->top, base + 1, &vlo, &vhi);
            ink_rows(r, &r->value, o->top, base + 1, &olo, &ohi);
            ink_rows(r, &r->label, c->label, base + 12, &llo, &lhi);
            int top = vhi > ohi ? vhi : ohi, floor_ = (int)lroundf((float)(base + 12) * s->scale);  // spark bottom row
            if (llo < floor_) floor_ = llo;
            if (top <= floor_) {
                int x0 = (int)floor(x * s->scale), x1 = (int)ceil((x + w) * s->scale);
                if (x0 < 0) x0 = 0;
                if (x1 > px->pw) x1 = px->pw;
                if (vchg) {
                    if (top > px->ph) top = px->ph;
                    for (int y = 0; y < top; y++) memset(px->buf + (size_t)(px->ph - 1 - y) * px->stride + x0, 0, (size_t)(x1 - x0) * 4);
                    text(r, ctx, t, &r->value, c->top, x, base + 1, c->warn ? &r->lut_warn : lfg);
                }
                if (schg) {
                    chart_geo_t g;
                    spark_geo(&g, c, s, x, base);
                    int keep = 0, kk = chart_shift(o, c, &keep);
                    if (kk >= 0) scroll_chart(px, o, c, &g, kk, keep);
                    else {
                        for (int y = g.iy; y < g.iy + g.ih; y++) {
                            int row = px->ph - 1 - y;
                            if (row >= 0 && row < px->ph) memset(px->buf + (size_t)row * px->stride + g.bx, 0, (size_t)g.bw * 4);
                        }
                        chart_series(px, c, &g, 0, g.n);
                    }
                }
                r->values_only++;
                x += w + GAP;
                continue;
            }
        }
        if (dirty && px) {  // clear this cell's columns (whole height)
            int x0 = (int)floor(x * s->scale), x1 = (int)ceil((x + w) * s->scale);
            if (x0 < 0) x0 = 0;
            if (x1 > px->pw) x1 = px->pw;
            for (int row = 0; row < px->ph && x1 > x0; row++) memset(px->buf + (size_t)row * px->stride + x0, 0, (size_t)(x1 - x0) * 4);
        }
        if (c->kind == CELL_MINI) {
            const colorlut_t *lv = c->warn ? &r->lut_warn : lfg;
            if (s->labels) {
                text(r, ctx, t, &r->label, c->label, x, base + 12, lfg);
                SET_FILL(c->warn ? warn : fg);
                text(r, ctx, t, &r->value, c->top, x, base + 1, lv);
                if (spark && px) {
                    chart_geo_t g;
                    spark_geo(&g, c, s, x, base);
                    chart_series(px, c, &g, 0, g.n);
                }
            } else {
                SET_FILL(c->warn ? warn : fg);
                CGFloat tw = glyphset_width(&r->value, c->top);
                text(r, ctx, t, &r->value, c->top, x + (w - tw) / 2, base + 4, lv);
            }
            SET_FILL(fg);
        } else if (c->kind == CELL_SPEED) {
            // two right-aligned rows, arrows on the left
            CGFloat tw = glyphset_width(&r->small, c->top), bw = glyphset_width(&r->small, c->bottom);
            text(r, ctx, t, &r->small, "\x02", x, base + 9.5, lfg);
            text(r, ctx, t, &r->small, "\x03", x, base + 0.5, lfg);
            text(r, ctx, t, &r->small, c->top, x + w - tw, base + 9.5, lfg);
            text(r, ctx, t, &r->small, c->bottom, x + w - bw, base + 0.5, lfg);
        } else if (c->kind == CELL_BATTERY) {
            draw_battery(ctx, r, c, x, base + 9, fg);
        } else if (c->kind == CELL_CHART || c->kind == CELL_NETCHART) {
            if (px) draw_chart(px, c, s, x, base);
        }
        x += w + GAP;
    }
}

CGImageRef renderer_draw(renderer_t *r, const strip_t *s, CGFloat width) {
    int pw = (int)ceil(width * s->scale), ph = (int)ceil(s->height * s->scale);
    if (pw <= 0 || ph <= 0) return NULL;
    int exact = r->mode == RENDER_IMAGE_EXACT || r->mode == RENDER_BLIT_IMAGE;
    ensure_ctx(r, pw, ph, s->scale, exact);
    target_t t = {r->pixels, (size_t)r->pw, r->pw, ph};
    r->cur_target = &t;
    paint(r, r->ctx, blitting(r) ? &t : NULL, s, r->pw / s->scale, NULL, NULL);
    r->cur_target = NULL;
    CGImageRef full = CGBitmapContextCreateImage(r->ctx);
    if (!full || (int)CGImageGetWidth(full) == pw) return full;
    // RENDER_IMAGE_CROP: the backing store is wider than needed; crop (a copy).
    CGImageRef crop = CGImageCreateWithImageInRect(full, CGRectMake(0, 0, pw, ph));
    CGImageRelease(full);
    return crop;
}

static int make_surface(renderer_t *r, int i, int pw, int ph, float scale) {
    int bpe = 4, fmt = 'BGRA';
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &pw), h = CFNumberCreate(NULL, kCFNumberIntType, &ph);
    CFNumberRef b = CFNumberCreate(NULL, kCFNumberIntType, &bpe), f = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *k[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    const void *v[] = {w, h, b, f};
    CFDictionaryRef props = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    r->surf[i] = IOSurfaceCreate(props);
    CFRelease(props); CFRelease(w); CFRelease(h); CFRelease(b); CFRelease(f);
    if (!r->surf[i]) return 0;
    if (IOSurfaceLock(r->surf[i], 0, NULL) != kIOReturnSuccess) { CFRelease(r->surf[i]); r->surf[i] = NULL; return 0; }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    r->sctx[i] = CGBitmapContextCreate(IOSurfaceGetBaseAddress(r->surf[i]), (size_t)pw, (size_t)ph, 8,
                                       IOSurfaceGetBytesPerRow(r->surf[i]), cs,
                                       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    IOSurfaceUnlock(r->surf[i], 0, NULL);
    if (!r->sctx[i]) { CFRelease(r->surf[i]); r->surf[i] = NULL; return 0; }
    CGContextScaleCTM(r->sctx[i], scale, scale);
    CGContextSetTextMatrix(r->sctx[i], CGAffineTransformIdentity);
    CGContextSetShouldSubpixelPositionFonts(r->sctx[i], true);
    CGContextSetShouldSubpixelQuantizeFonts(r->sctx[i], true);
    return 1;
}

// Two strips that paint cells at the same places in the same way.
static int same_frame(const strip_t *a, const strip_t *b) {
    if (a->ncells != b->ncells || a->dark != b->dark || a->labels != b->labels || a->scale != b->scale || a->height != b->height ||
        a->mono != b->mono || memcmp(a->accent, b->accent, sizeof b->accent) != 0)
        return 0;
    for (int i = 0; i < b->ncells; i++)
        if (a->cell[i].kind != b->cell[i].kind || cell_width(&a->cell[i], a->labels) != cell_width(&b->cell[i], b->labels)) return 0;
    return 1;
}

// Draws only into a pooled surface the compositor is not using (IOSurfaceIsInUse).
// Returns NULL (skip this frame) when none is free or allocation failed.
IOSurfaceRef renderer_draw_surface(renderer_t *r, const strip_t *s, CGFloat width) {
    int pw = (int)ceil(width * s->scale), ph = (int)ceil(s->height * s->scale);
    if (pw <= 0 || ph <= 0) return NULL;
    if (r->sw != pw || r->sh != ph || r->sscale != s->scale) {
        for (int i = 0; i < SURF_POOL; i++) {
            if (r->sctx[i]) CGContextRelease(r->sctx[i]);
            if (r->surf[i]) CFRelease(r->surf[i]);
            r->sctx[i] = NULL;
            r->surf[i] = NULL;
        }
        r->sw = pw;
        r->sh = ph;
        r->sscale = s->scale;
        memset(r->surf_valid, 0, sizeof r->surf_valid);
    }
    // Most recently painted free surface (fewest cells to catch up).
    int pick = -1, empty = -1;
    for (int i = 0; i < SURF_POOL; i++) {
        if (!r->surf[i]) { if (empty < 0) empty = i; continue; }
        if (IOSurfaceIsInUse(r->surf[i])) continue;
        if (pick < 0 || r->surf_painted[i] > r->surf_painted[pick]) pick = i;
    }
    if (pick < 0 && empty >= 0 && make_surface(r, empty, pw, ph, s->scale)) pick = empty;
    if (pick < 0) return NULL;
    IOSurfaceRef sf = r->surf[pick];
    if (IOSurfaceLock(sf, 0, NULL) != kIOReturnSuccess) return NULL;
    r->cur = pick;
    target_t t = {IOSurfaceGetBaseAddress(sf), IOSurfaceGetBytesPerRow(sf) / 4, pw, ph};
    r->cur_target = &t;
    // Repaint only the cells that differ from what this surface showed last.
    uint8_t dirty[STRIP_MAX_CELLS];
    const strip_t *was = &r->surf_strip[pick];
    int partial = blitting(r) && r->surf_valid[pick] && same_frame(was, s);
    for (int i = 0; partial && i < s->ncells; i++) dirty[i] = (uint8_t)(memcmp(&was->cell[i], &s->cell[i], sizeof(cell_t)) != 0);
    paint(r, r->sctx[pick], blitting(r) ? &t : NULL, s, width, partial ? dirty : NULL, partial ? was : NULL);
    r->surf_strip[pick] = *s;
    r->surf_valid[pick] = 1;
    r->surf_painted[pick] = ++r->paint_count;
    r->cur_target = NULL;
    CGContextFlush(r->sctx[pick]);
    IOSurfaceUnlock(sf, 0, NULL);
    return (IOSurfaceRef)CFRetain(sf);
}
