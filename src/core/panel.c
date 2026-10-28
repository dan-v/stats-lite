#include "panel.h"

#include <IOSurface/IOSurface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define M 14.0f     // horizontal margin, points
#define TOP 12.0f
#define BOTTOM 10.0f

static float item_height(const pitem_t *it) {
    switch (it->kind) {
    case PI_HEADER: return 20;
    case PI_ROW: return 16;
    case PI_SMALL: case PI_CAPTION: return 14;
    case PI_SPARK: return 40;
    case PI_DAY: return 28;
    case PI_CORES: return 30;
    case PI_BAR: return 12;
    case PI_DIVIDER: return 12;
    case PI_GAP: return it->h;
    case PI_NOTE: return 14;
    case PI_PROC: return 18;
    case PI_STACK: return 14;
    case PI_KV: return 14;
    }
    return 0;
}

float panel_measure(const plist_t *l) {
    float y = TOP;
    for (int i = 0; i < l->n; i++) y += item_height(&l->item[i]);
    return y + BOTTOM;
}

// ---- builders --------------------------------------------------------------------
void pl_reset(plist_t *l) { l->n = 0; }
static pitem_t *add(plist_t *l, int kind) {
    if (l->n >= PANEL_MAX_ITEMS) return NULL;
    pitem_t *it = &l->item[l->n++];
    memset(it, 0, offsetof(pitem_t, v));  // v/tag are filled by the caller up to n
    it->kind = (uint8_t)kind;
    return it;
}
// strlcpy that never ends in a partial UTF-8 sequence (CoreText rejects those).
static void utf8_copy(char *dst, const char *src, size_t cap) {
    size_t n = strlcpy(dst, src, cap);
    if (n < cap) return;
    n = cap - 1;
    size_t i = n;
    while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) i--;  // back to the last lead byte
    if (i > 0) {
        unsigned char lead = (unsigned char)dst[i - 1];
        size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (i - 1 + len > n) n = i - 1;  // that character was cut: drop it
    }
    dst[n] = 0;
}

static void set2(pitem_t *it, const char *a, const char *b) {
    if (!it) return;
    if (a) utf8_copy(it->a, a, sizeof it->a);
    if (b) utf8_copy(it->b, b, sizeof it->b);
}
void pl_header(plist_t *l, const char *t, const char *v) { set2(add(l, PI_HEADER), t, v); }
void pl_row(plist_t *l, const char *a, const char *b) { set2(add(l, PI_ROW), a, b); }
void pl_small(plist_t *l, const char *a, const char *b) { set2(add(l, PI_SMALL), a, b); }
void pl_caption(plist_t *l, const char *a) { set2(add(l, PI_CAPTION), a, NULL); }
pitem_t *pl_spark(plist_t *l, int c, int c2) {
    pitem_t *it = add(l, PI_SPARK);
    if (it) { it->color = (uint8_t)c; it->color2 = (uint8_t)c2; it->nseries = 1; }
    return it;
}
pitem_t *pl_day(plist_t *l, int c) {
    pitem_t *it = add(l, PI_DAY);
    if (it) { it->color = (uint8_t)c; it->nseries = 2; }
    return it;
}
pitem_t *pl_cores(plist_t *l) {
    pitem_t *it = add(l, PI_CORES);
    if (it) it->nseries = 1;  // values are part of the change hash
    return it;
}
void pl_bar(plist_t *l, float f, int c) {
    pitem_t *it = add(l, PI_BAR);
    if (it) { it->color = (uint8_t)c; it->n = 1; it->nseries = 1; it->v[0][0] = f; }
}
void pl_divider(plist_t *l) { add(l, PI_DIVIDER); }
void pl_gap(plist_t *l, float h) {
    pitem_t *it = add(l, PI_GAP);
    if (it) it->h = h;
}
void pl_note(plist_t *l, const char *a) { set2(add(l, PI_NOTE), a, NULL); }
void pl_proc(plist_t *l, const char *name, const char *value, float share, int c) {
    pitem_t *it = add(l, PI_PROC);
    if (!it) return;
    set2(it, name, value);
    it->color = (uint8_t)c;
    it->n = 1;
    it->nseries = 1;
    it->v[0][0] = share;
}
void pl_kv(plist_t *l, const char *labels, const char *values, const uint8_t *dots, const float *offsets) {
    pitem_t *it = add(l, PI_KV);
    if (!it) return;
    set2(it, labels, values);
    int n = 1;
    for (const char *q = it->a; *q; q++) n += *q == '|';
    it->n = (uint8_t)(n > 4 ? 4 : n);
    for (int i = 0; i < it->n; i++) {
        it->tag[i] = dots ? dots[i] : 0xff;
        it->v[0][i] = offsets ? offsets[i] : 0;
    }
}
pitem_t *pl_stack(plist_t *l) {
    pitem_t *it = add(l, PI_STACK);
    if (it) it->nseries = 1;
    return it;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
// Static content: what goes on the static layer (labels, geometry).
static uint64_t static_hash(const pitem_t *it) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t k[3] = {it->kind, it->kind == PI_CORES ? it->n : 0, it->color};
    h = fnv(h, k, sizeof k);
    h = fnv(h, &it->h, sizeof it->h);
    if (it->kind == PI_HEADER || it->kind == PI_ROW || it->kind == PI_CAPTION || it->kind == PI_KV) h = fnv(h, it->a, strlen(it->a) + 1);
    if (it->kind == PI_KV) { h = fnv(h, it->tag, it->n); h = fnv(h, it->v[0], sizeof(float) * it->n); h = fnv(h, &it->n, 1); }
    return h;
}
// Dynamic content: values and chart ink.
static uint64_t item_hash(const pitem_t *it) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, it, offsetof(pitem_t, a));
    if (it->kind == PI_SMALL || it->kind == PI_NOTE || it->kind == PI_PROC) h = fnv(h, it->a, strlen(it->a) + 1);
    h = fnv(h, it->b, strlen(it->b) + 1);
    for (int s = 0; s < it->nseries && s < 2; s++) {
        // quantize to 1/1024 so float noise below a pixel doesn't force a repaint
        for (int i = 0; i < it->n; i++) {
            int16_t q = (int16_t)lroundf(it->v[s][i] * 1024);
            h = fnv(h, &q, 2);
        }
    }
    if (it->kind == PI_CORES || it->kind == PI_STACK) h = fnv(h, it->tag, it->n);
    return h;
}

// ---- setup -----------------------------------------------------------------------
static CTFontRef font(CGFloat size, CGFloat weight, int monodigits) {
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberCGFloatType, &weight);
    const void *tk[] = {kCTFontWeightTrait};
    const void *tv[] = {w};
    CFDictionaryRef traits = CFDictionaryCreate(NULL, tk, tv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);
    if (monodigits) {
        int t = 6, sel = 0;
        CFNumberRef nt = CFNumberCreate(NULL, kCFNumberIntType, &t), ns = CFNumberCreate(NULL, kCFNumberIntType, &sel);
        const void *fk[] = {kCTFontFeatureTypeIdentifierKey, kCTFontFeatureSelectorIdentifierKey};
        const void *fv[] = {nt, ns};
        CFDictionaryRef feat = CFDictionaryCreate(NULL, fk, fv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayRef feats = CFArrayCreate(NULL, (const void **)&feat, 1, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, feats);
        CFRelease(feats); CFRelease(feat); CFRelease(nt); CFRelease(ns);
    }
    CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
    CTFontRef f = CTFontCreateCopyWithAttributes(base, size, NULL, d);
    CFRelease(d); CFRelease(attrs); CFRelease(traits); CFRelease(w); CFRelease(base);
    return f;
}

void panel_init(panel_t *p) {
    memset(p, 0, sizeof *p);
    CTFontRef t = font(13, 0.3, 1), v = font(11, 0, 1), lb = font(11, 0, 0), sm = font(10, 0, 1);
    glyphset_init(&p->title, t);
    glyphset_init(&p->value, v);
    glyphset_init(&p->label, lb);
    glyphset_init(&p->small, sm);
    CFRelease(t); CFRelease(v); CFRelease(lb); CFRelease(sm);
    // Defaults; the app overrides them with the resolved system colors.
    colorlut_init(&p->lut_primary[1], 1, 1, 1, 0.85f);
    colorlut_init(&p->lut_secondary[1], 1, 1, 1, 0.55f);
    colorlut_init(&p->lut_primary[0], 0, 0, 0, 0.85f);
    colorlut_init(&p->lut_secondary[0], 0, 0, 0, 0.5f);
    const float pal[PC_COUNT][3] = {{0.04f, 0.52f, 1}, {0.75f, 0.35f, 0.95f}, {0.2f, 0.78f, 0.35f}, {0.19f, 0.69f, 0.78f},
                                    {1, 0.22f, 0.37f}, {1, 0.62f, 0.04f}, {1, 0.27f, 0.23f}, {0.04f, 0.52f, 1}};
    for (int d = 0; d < 2; d++)
        for (int k = 0; k < PC_COUNT; k++) p->palette[d][k] = premul_rgba(pal[k][0], pal[k][1], pal[k][2], 1);
}

void panel_release(panel_t *p) {
    for (int i = 0; i < 3; i++) {
        if (p->ctx[i]) CGContextRelease(p->ctx[i]);
        if (p->surf[i]) CFRelease(p->surf[i]);
        p->ctx[i] = NULL;
        p->surf[i] = NULL;
        p->surf_frame[i] = 0;
    }
    for (int i = 0; i < 2; i++) {
        if (p->sctx[i]) CGContextRelease(p->sctx[i]);
        if (p->stat[i]) CFRelease(p->stat[i]);
        p->sctx[i] = NULL;
        p->stat[i] = NULL;
    }
    p->pw = p->ph = 0;
    p->nprev = 0;
    p->static_layout = 0;
}

// ---- drawing helpers (y in points from the panel top; converted to device px) ----
typedef struct {
    panel_t *p;
    target_t t;
    CGContextRef cg;
    float s;      // scale
    float scroll;
    int dark;
    int clip0, clip1;  // spark column clip (device px), 0 = none
    int ix0, ix1;      // device columns the current item's dynamic text may have inked
} dc_t;

static inline int dev_y_bottom(const dc_t *c, float y_top_pt) {  // device row index from the bottom
    return c->t.ph - (int)lroundf((y_top_pt - c->scroll) * c->s);
}

static int ascii(const char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 128) return 0;
    return 1;
}

// Text with its top at y (points). right: x is the right edge. Truncates to max_w.
// Text ink stays within its advance plus a small overhang (anti-aliasing, bearings).
static inline void ink_span(dc_t *c, float x0, float x1) {
    int a = (int)floorf((x0 - 3) * c->s), b = (int)ceilf((x1 + 3) * c->s);
    if (a < 0) a = 0;
    if (b > c->t.pw) b = c->t.pw;
    if (a < c->ix0) c->ix0 = a;
    if (b > c->ix1) c->ix1 = b;
}

static void text(dc_t *c, glyphset_t *g, const char *str, float x, float y, int right, float max_w, const colorlut_t *lut,
                 int secondary) {
    char buf[64];
    utf8_copy(buf, str, sizeof buf);
    if (!ascii(buf)) {  // CoreText fallback for non-ASCII (e.g. localized app names)
        float rgba[4];
        uint32_t px = lut->px[255];
        rgba[3] = (px >> 24) / 255.0f;
        rgba[0] = rgba[3] > 0 ? ((px >> 16) & 255) / 255.0f / rgba[3] : 0;
        rgba[1] = rgba[3] > 0 ? ((px >> 8) & 255) / 255.0f / rgba[3] : 0;
        rgba[2] = rgba[3] > 0 ? (px & 255) / 255.0f / rgba[3] : 0;
        CGColorRef col = CGColorCreateSRGB(rgba[0], rgba[1], rgba[2], rgba[3]);
        CFStringRef s = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
        if (!s) { CGColorRelease(col); return; }
        const void *k[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
        const void *v[] = {g->font, col};
        CFDictionaryRef at = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(NULL, s, at);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        if (w > max_w) {
            CFAttributedStringRef ell = CFAttributedStringCreate(NULL, CFSTR("…"), at);
            CTLineRef tok = CTLineCreateWithAttributedString(ell);
            CTLineRef tr = CTLineCreateTruncatedLine(line, max_w, kCTLineTruncationEnd, tok);
            CFRelease(tok); CFRelease(ell);
            if (tr) { CFRelease(line); line = tr; w = CTLineGetTypographicBounds(line, NULL, NULL, NULL); }
        }
        float x0 = right ? x - (float)w : x;
        ink_span(c, x0, x0 + (float)w);
        float base_pt = (float)c->t.ph / c->s - (y - c->scroll + (float)ceil(g->ascent));
        CGContextSetTextPosition(c->cg, x0, base_pt);
        CTLineDraw(line, c->cg);
        CFRelease(line); CFRelease(as); CFRelease(at); CFRelease(s); CGColorRelease(col);
        return;
    }
    float w = (float)glyphset_width(g, buf);
    if (w > max_w) {  // drop characters until "name.." fits
        size_t n = strlen(buf);
        float dots = (float)glyphset_width(g, "..");
        while (n > 1 && (float)glyphset_width(g, buf) + dots > max_w) buf[--n] = 0;
        strlcat(buf, "..", sizeof buf);
        w = (float)glyphset_width(g, buf);
    }
    (void)secondary;
    float x0 = right ? x - w : x;
    ink_span(c, x0, x0 + w);
    int dark_ink = !c->dark;
    atlas_t *a = &g->atlas[dark_ink];
    if (a->scale != c->s || !a->pix) atlas_build(a, g->font, g->glyph, c->s, dark_ink);
    int baseline = c->t.ph - (int)lroundf((y - c->scroll + (float)ceil(g->ascent)) * c->s);
    BLIT_TEXT(&c->t, a, buf, x0 * c->s, baseline, lut);
}

static uint32_t with_alpha(uint32_t premul_opaque, float a) {
    uint32_t a8 = (uint32_t)(a * 255 + 0.5f);
    uint32_t r = ((premul_opaque >> 16) & 255) * a8 / 255, g = ((premul_opaque >> 8) & 255) * a8 / 255, b = (premul_opaque & 255) * a8 / 255;
    return a8 << 24 | r << 16 | g << 8 | b;
}

static uint32_t ink(const dc_t *c, float a) {  // the primary text color at alpha a
    float v = c->dark ? 1 : 0;
    return premul_rgba(v, v, v, a);
}

// Coverage of a rounded rectangle (w x h, radius r) at pixel (xx, yy) inside it.
// Only pixels inside a corner square need the distance math.
static inline float rr_cov(int xx, int yy, int w, int h, float r) {
    if ((float)yy >= r && (float)yy < (float)h - r) return 1;
    if ((float)xx >= r && (float)xx < (float)w - r) return 1;
    float fx = xx + 0.5f, fy = yy + 0.5f;
    float cx = fx < r ? r : fx > w - r ? w - r : fx, cy = fy < r ? r : fy > h - r ? h - r : fy;
    if (cx == fx || cy == fy) return 1;
    float cov = r + 0.5f - sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
    return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

// Area + anti-aliased line chart of one series inside a box (device px) on the
// transparent dynamic layer. Only the line, the area's top pixel and corners blend.
static void area_line(const dc_t *c, int bx, int by, int bw, int bh, const float *v, int n, int cap, uint32_t col, float radius,
                      int clip0, int clip1) {
    if (n < 2) return;
    const target_t *t = &c->t;
    float dx = (float)bw / (float)(cap - 1);
    float x0 = (float)(bx + bw) - dx * (float)(n - 1);
    float hw = 0.6f * c->s;  // half line width: 1.2 pt
    uint32_t fill = with_alpha(col, 0.2f), line = col;
    int corner = (int)ceilf(radius);
    int xa = (int)floorf(x0), xb = bx + bw;
    for (int x = xa < bx ? bx : xa; x < xb && x < t->pw; x++) {
        if (x < 0 || x < clip0 || x >= clip1) continue;
        float fx = (float)x + 0.5f;
        float pos = (fx - x0) / dx;
        int i = (int)floorf(pos);
        if (i < 0) i = 0;
        if (i > n - 2) i = n - 2;
        float tt = pos - (float)i;
        float ya = (float)by + 1 + (float)(bh - 2) * fminf(fmaxf(v[i], 0), 1);
        float yb = (float)by + 1 + (float)(bh - 2) * fminf(fmaxf(v[i + 1], 0), 1);
        float yc = ya + (yb - ya) * tt;
        int top = (int)floorf(yc);
        int edge = x < bx + corner || x >= xb - corner;
        uint32_t *base = t->buf + (size_t)x;
        int ylo = by < 0 ? 0 : by, yhi = top > t->ph ? t->ph : top;
        if (edge) {
            for (int y = ylo; y < yhi; y++) {
                float cv = rr_cov(x - bx, y - by, bw, bh, radius);
                if (cv >= 1) { uint32_t *d = base + (size_t)(t->ph - 1 - y) * t->stride; *d = *d ? blit_blend(fill, *d) : fill; }
                else if (cv > 0) blit_put_cov(t, x, y, fill, cv);
            }
        } else {
            for (int y = ylo; y < yhi; y++) {
                uint32_t *d = base + (size_t)(t->ph - 1 - y) * t->stride;
                *d = *d ? blit_blend(fill, *d) : fill;
            }
        }
        blit_put_cov(t, x, top, fill, (yc - (float)top) * (edge ? rr_cov(x - bx, top - by, bw, bh, radius) : 1));
        float sx = x0 + dx * (float)i, ex = sx + dx;
        float lo = fminf(ya, yb) - hw - 1, hi = fmaxf(ya, yb) + hw + 1;
        float segx = ex - sx, segy = yb - ya, len2 = segx * segx + segy * segy, inv = len2 > 0 ? 1 / len2 : 0;
        for (int y = (int)floorf(lo); y <= (int)ceilf(hi); y++) {
            if (y < by || y >= by + bh) continue;
            float py = (float)y + 0.5f;
            float u = ((fx - sx) * segx + (py - ya) * segy) * inv;
            u = fminf(fmaxf(u, 0), 1);
            float qx = sx + u * segx - fx, qy = ya + u * segy - py;
            float cov = hw + 0.5f - sqrtf(qx * qx + qy * qy);
            if (cov > 0) blit_put_cov(t, x, y, line, (cov > 1 ? 1 : cov) * (edge ? rr_cov(x - bx, y - by, bw, bh, radius) : 1));
        }
    }
}

enum { PART_STATIC = 1, PART_DYNAMIC = 2 };

// Draws the static (labels, backgrounds, dividers) or dynamic (values, chart ink)
// part of a row. The two parts go to different layers.
static void draw_item(dc_t *c, const pitem_t *it, float y, int part) {
    panel_t *p = c->p;
    int d = c->dark, st = part & PART_STATIC, dy = part & PART_DYNAMIC;
    float s = c->s, W = PANEL_W;
    const colorlut_t *pri = &p->lut_primary[d], *sec = &p->lut_secondary[d];
    switch (it->kind) {
    case PI_HEADER:
        if (st) text(c, &p->title, it->a, M, y, 0, 150, pri, 0);
        if (dy && it->b[0]) text(c, &p->title, it->b, W - M, y, 1, 120, pri, 0);
        break;
    case PI_ROW:
        if (st) text(c, &p->label, it->a, M, y, 0, 150, sec, 1);
        if (dy) text(c, &p->value, it->b, W - M, y, 1, 150, pri, 0);
        break;
    case PI_SMALL:
        if (dy) {
            text(c, &p->small, it->a, M, y, 0, 150, sec, 1);
            text(c, &p->small, it->b, W - M, y, 1, 110, pri, 0);
        }
        break;
    case PI_CAPTION:
        if (st) text(c, &p->small, it->a, M, y, 0, W - 2 * M, sec, 1);
        break;
    case PI_NOTE:
        if (dy) text(c, &p->small, it->a, M, y, 0, W - 2 * M, sec, 1);
        break;
    case PI_PROC: {
        // name and value, with the share as a thin bar along the bottom of the row
        int bx = (int)lroundf(M * s), bw = (int)lroundf((W - 2 * M) * s), bh = (int)lroundf(2 * s);
        int by = dev_y_bottom(c, y + 17);
        // square ends: at 2 pt rounding is invisible, and a rounded rect this thin is all corner math
        if (st) blit_fill(&c->t, bx, by, bw, bh, ink(c, 0.07f));
        if (dy) {
            int fw = (int)lroundf(fminf(fmaxf(it->v[0][0], 0), 1) * (float)bw);
            if (fw > 0) blit_fill(&c->t, bx, by, fw < bh ? bh : fw, bh, p->palette[d][it->color]);
            text(c, &p->label, it->a, M, y, 0, 200, pri, 0);
            text(c, &p->value, it->b, W - M, y, 1, 80, pri, 0);
        }
        break;
    }
    case PI_KV: {
        char la[48], va[48];
        strlcpy(la, it->a, sizeof la);
        strlcpy(va, it->b, sizeof va);
        char *ln = la, *vn = va;
        float slot = (W - 2 * M) / (float)(it->n ? it->n : 1);
        for (int i = 0; i < it->n && ln; i++) {
            char *lb = strchr(ln, '|'), *vb = vn ? strchr(vn, '|') : NULL;
            if (lb) *lb = 0;
            if (vb) *vb = 0;
            float x = M + (it->v[0][i] > 0 || i == 0 ? it->v[0][i] : slot * (float)i);
            if (it->tag[i] != 0xff) {
                int dsz = (int)lroundf(6 * s);
                if (st) blit_round_rect(&c->t, (int)lroundf(x * s), dev_y_bottom(c, y + 10), dsz, dsz, 3 * s, p->palette[d][it->tag[i]]);
                x += 9;
            }
            if (st) text(c, &p->small, ln, x, y, 0, slot, sec, 1);
            if (dy && vn) text(c, &p->small, vn, x + (float)glyphset_width(&p->small, ln) + 4, y, 0, W - M - x, pri, 0);
            ln = lb ? lb + 1 : NULL;
            vn = vb ? vb + 1 : NULL;
        }
        break;
    }
    case PI_STACK: {
        int bx = (int)lroundf(M * s), bw = (int)lroundf((W - 2 * M) * s), bh = (int)lroundf(8 * s);
        int by = dev_y_bottom(c, y + 8);
        if (st) blit_round_rect(&c->t, bx, by, bw, bh, 3 * s, ink(c, 0.1f));
        if (dy) {
            float acc = 0;
            int gap = (int)lroundf(s);
            for (int i = 0; i < it->n; i++) {
                int x0 = bx + (int)lroundf(acc * (float)bw);
                acc += fminf(fmaxf(it->v[0][i], 0), 1);
                int x1 = bx + (int)lroundf(fminf(acc, 1) * (float)bw);
                if (x1 - x0 - gap < 2) continue;
                blit_round_rect(&c->t, x0, by, x1 - x0 - gap, bh, x1 - x0 > 6 * s ? 2 * s : 0, p->palette[d][it->tag[i]]);
            }
        }
        break;
    }
    case PI_SPARK: {
        int bx = (int)lroundf(M * s), bw = (int)lroundf((W - 2 * M) * s), bh = (int)lroundf(34 * s);
        int by = dev_y_bottom(c, y + 34);
        if (st) blit_round_rect(&c->t, bx, by, bw, bh, 4 * s, ink(c, 0.06f));
        if (dy) {
            int c0 = c->clip0 ? c->clip0 : bx, c1 = c->clip1 ? c->clip1 : bx + bw;
            if (it->nseries > 1) area_line(c, bx, by, bw, bh, it->v[1], it->n, HIST_CAP, p->palette[d][it->color2], 4 * s, c0, c1);
            area_line(c, bx, by, bw, bh, it->v[0], it->n, HIST_CAP, p->palette[d][it->color], 4 * s, c0, c1);
        }
        break;
    }
    case PI_DAY: {
        float lx = M + 28;
        int bx = (int)lroundf(lx * s), bw = (int)lroundf((W - M - lx) * s), bh = (int)lroundf(22 * s);
        int by = dev_y_bottom(c, y + 22);
        if (st) {
            text(c, &p->small, "24 h", M, y + 4, 0, 30, sec, 1);
            blit_round_rect(&c->t, bx, by, bw, bh, 3 * s, ink(c, 0.06f));
        }
        if (dy && it->n) {
            uint32_t col = p->palette[d][it->color];
            uint32_t ca = with_alpha(col, 0.85f), cm = with_alpha(col, 0.25f);
            for (int i = 0; i < it->n; i++) {
                int x1 = bx + (int)((float)i * (float)bw / (float)it->n), x2 = bx + (int)((float)(i + 1) * (float)bw / (float)it->n);
                float mx = it->v[1][i], av = it->v[0][i];
                if (mx < 0) continue;
                int hm = (int)lroundf(fminf(mx, 1) * (float)(bh - 2)), ha = (int)lroundf(fminf(av, 1) * (float)(bh - 2));
                int x2e = x2 - (x2 - x1 > 2 ? 1 : 0);
                if (ha > 0) blit_fill(&c->t, x1, by + 1, x2e - x1, ha, ca);
                if (hm > ha) blit_fill(&c->t, x1, by + 1 + ha, x2e - x1, hm - ha, cm);
            }
        }
        break;
    }
    case PI_CORES: {
        if (!it->n) break;
        // Up to 24 bars per row; more cores wrap into rows sharing the 24 pt height.
        int per_row = 24, rows = (it->n + per_row - 1) / per_row, cols = it->n < per_row ? it->n : per_row;
        float gap = 2, bwpt = (W - 2 * M - gap * (float)(cols - 1)) / (float)cols;
        float rh = (24 - gap * (float)(rows - 1)) / (float)rows;
        for (int i = 0; i < it->n; i++) {
            int r = i / per_row, k = i % per_row;
            float top = y + (float)r * (rh + gap);
            int bh = (int)lroundf(rh * s), by = dev_y_bottom(c, top + rh);
            int x1 = (int)lroundf((M + (float)k * (bwpt + gap)) * s), x2 = (int)lroundf((M + (float)k * (bwpt + gap) + bwpt) * s);
            if (st) blit_round_rect(&c->t, x1, by, x2 - x1, bh, 1.5f * s, ink(c, 0.08f));
            if (dy) {
                int h = (int)lroundf(fminf(fmaxf(it->v[0][i], 0), 1) * (float)bh);
                uint32_t col = p->palette[d][it->tag[i] ? PC_ORANGE : PC_BLUE];
                if (h > 0) blit_round_rect(&c->t, x1, by, x2 - x1, h, h > 3 * s ? 1.5f * s : 0, col);
            }
        }
        break;
    }
    case PI_BAR: {
        int bx = (int)lroundf(M * s), bw = (int)lroundf((W - 2 * M) * s), bh = (int)lroundf(6 * s);
        int by = dev_y_bottom(c, y + 6);
        if (st) blit_round_rect(&c->t, bx, by, bw, bh, 3 * s, ink(c, 0.1f));
        int fw = (int)lroundf(fminf(fmaxf(it->v[0][0], 0), 1) * (float)bw);
        if (dy && fw > 0) blit_round_rect(&c->t, bx, by, fw, bh, 3 * s, p->palette[d][it->color]);
        break;
    }
    case PI_DIVIDER:
        if (st) {
            int bx = (int)lroundf(M * s), bw = (int)lroundf((W - 2 * M) * s);
            blit_fill(&c->t, bx, dev_y_bottom(c, y + 5), bw, (int)fmaxf(1, roundf(s * 0.5f)), ink(c, 0.12f));
        }
        break;
    }
}

// ---- surfaces ---------------------------------------------------------------------
static int make_surface(IOSurfaceRef *sf, CGContextRef *ctx, int pw, int ph, float scale) {
    int bpe = 4, fmt = 'BGRA';
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &pw), h = CFNumberCreate(NULL, kCFNumberIntType, &ph);
    CFNumberRef b = CFNumberCreate(NULL, kCFNumberIntType, &bpe), f = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *k[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    const void *v[] = {w, h, b, f};
    CFDictionaryRef props = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    *sf = IOSurfaceCreate(props);
    CFRelease(props); CFRelease(w); CFRelease(h); CFRelease(b); CFRelease(f);
    if (!*sf) return 0;
    if (IOSurfaceLock(*sf, 0, NULL) != kIOReturnSuccess) { CFRelease(*sf); *sf = NULL; return 0; }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    *ctx = CGBitmapContextCreate(IOSurfaceGetBaseAddress(*sf), (size_t)pw, (size_t)ph, 8, IOSurfaceGetBytesPerRow(*sf), cs,
                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    IOSurfaceUnlock(*sf, 0, NULL);
    if (!*ctx) { CFRelease(*sf); *sf = NULL; return 0; }
    CGContextScaleCTM(*ctx, scale, scale);
    CGContextSetTextMatrix(*ctx, CGAffineTransformIdentity);
    return 1;
}

// Picks the most recently painted surface the compositor has released (least to
// catch up); creates a new one only when none is free.
static int pick_slot(IOSurfaceRef *sf, CGContextRef *ctx, const uint32_t *painted_frame, int n, int pw, int ph, float scale) {
    int best = -1, empty = -1;
    for (int i = 0; i < n; i++) {
        if (!sf[i]) { if (empty < 0) empty = i; continue; }
        if (IOSurfaceIsInUse(sf[i])) continue;
        if (best < 0 || (painted_frame && painted_frame[i] > painted_frame[best])) best = i;
    }
    if (best >= 0) return best;
    if (empty >= 0 && make_surface(&sf[empty], &ctx[empty], pw, ph, scale)) return empty;
    return -1;
}

// Clears device rows [y0, y1) (from the bottom), columns [x0, x1).
static void clear_band(const target_t *t, int y0, int y1, int x0, int x1) {
    if (y0 < 0) y0 = 0;
    if (y1 > t->ph) y1 = t->ph;
    if (x0 < 0) x0 = 0;
    if (x1 > t->pw) x1 = t->pw;
    if (x1 <= x0) return;
    for (int y = y0; y < y1; y++) memset(t->buf + (size_t)(t->ph - 1 - y) * t->stride + x0, 0, (size_t)(x1 - x0) * 4);
}

// Items whose dynamic part is text only; others may ink their whole width.
static int text_only(int kind) {
    return kind == PI_HEADER || kind == PI_ROW || kind == PI_SMALL || kind == PI_CAPTION || kind == PI_NOTE || kind == PI_KV;
}

// Device rows (from the box bottom) that can hold ink: the highest sample plus the
// line's half width and anti-aliasing margin.
static int spark_inktop(const pitem_t *it, int bh, float s) {
    float mx = 0;
    for (int k = 0; k < it->nseries && k < 2; k++)
        for (int i = 0; i < it->n; i++) mx = fmaxf(mx, it->v[k][i]);
    int top = 1 + (int)ceilf((float)(bh - 2) * fminf(mx, 1)) + (int)ceilf(0.6f * s) + 2;
    return top > bh ? bh : top;
}

#ifdef PANEL_PROFILE
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
uint64_t g_pp[4];
static uint64_t pp_now(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }
#define PP(k, stmt) do { uint64_t _a = pp_now(); stmt; g_pp[k] += pp_now() - _a; } while (0)
#else
#define PP(k, stmt) stmt
#endif

// Scrolls a sparkline's ink left by the samples added since this surface last showed
// it and redraws only the newest and corner columns. Returns 0 when a full redraw is needed.
static int spark_scroll(panel_t *p, dc_t *c, int surf, int i, const pitem_t *it, float top) {
    if (!p->sp_valid[surf][i] || p->sp_scale[surf][i] != it->vscale || it->serial <= p->sp_serial[surf][i]) return 0;
    float s = c->s;
    int bx = (int)lroundf(M * s), bw = (int)lroundf((PANEL_W - 2 * M) * s), bh = (int)lroundf(34 * s);
    int by = dev_y_bottom(c, top + 34);
    int dxp = bw / (HIST_CAP - 1);
    if (dxp * (HIST_CAP - 1) != bw) return 0;  // non-integral spacing at this scale
    uint32_t delta = it->serial - p->sp_serial[surf][i];
    int corner = (int)ceilf(4 * s) + 1;
    // Redraw margin: the line joint (2 pt) and columns leaving the right rounded corner.
    int shift = (int)delta * dxp, margin = (int)ceilf(2 * s) + 2;
    if (margin < corner) margin = corner;
    if (shift + margin + corner >= bw) return 0;
    const target_t *t = &c->t;
    // Only rows that can hold ink (old or new) move; rows above both are empty.
    int newtop = spark_inktop(it, bh, s), oldtop = p->sp_inktop[surf][i];
    int rows = newtop > oldtop ? newtop : oldtop;
    PP(0, for (int y = by; y < by + rows; y++) {
        if (y < 0 || y >= t->ph) continue;
        uint32_t *row = t->buf + (size_t)(t->ph - 1 - y) * t->stride;
        memmove(row + bx, row + bx + shift, (size_t)(bw - shift) * 4);
        memset(row + bx + bw - shift - margin, 0, (size_t)(shift + margin) * 4);  // newest columns
        memset(row + bx, 0, (size_t)corner * 4);                                  // left corner columns
    });
    c->clip0 = bx + bw - shift - margin;
    c->clip1 = bx + bw;
    PP(1, draw_item(c, it, top, PART_DYNAMIC));
    c->clip0 = bx;
    c->clip1 = bx + corner;
    PP(2, draw_item(c, it, top, PART_DYNAMIC));
    c->clip0 = c->clip1 = 0;
    p->sp_serial[surf][i] = it->serial;
    p->sp_inktop[surf][i] = (uint8_t)newtop;
    p->last_scrolled++;
    return 1;
}

IOSurfaceRef panel_render(panel_t *p, const plist_t *l, float height, float scale, int dark, float scroll, int *painted,
                          IOSurfaceRef *static_out) {
    int pw = (int)lroundf(PANEL_W * scale), ph = (int)lroundf(height * scale);
    *painted = 0;
    *static_out = NULL;
    if (pw <= 0 || ph <= 0) return NULL;
    p->frame++;
    int relayout = pw != p->pw || ph != p->ph || scale != p->scale || dark != p->dark || scroll != p->scroll || l->n != p->nprev;
    if (pw != p->pw || ph != p->ph || scale != p->scale) {
        panel_release(p);
        p->pw = pw;
        p->ph = ph;
        p->scale = scale;
    }
    p->dark = (uint8_t)dark;
    p->scroll = scroll;
    // Diff the display list: static hash/geometry changes rebuild the static layer.
    float y = TOP;
    for (int i = 0; i < l->n; i++) {
        uint64_t sh = static_hash(&l->item[i]), h = item_hash(&l->item[i]);
        if (i >= p->nprev || sh != p->shash[i] || y != p->ypos[i]) relayout = 1;
        if (i >= p->nprev || h != p->hash[i]) p->changed[i] = p->frame;
        p->shash[i] = sh;
        p->hash[i] = h;
        p->ypos[i] = y;
        y += item_height(&l->item[i]);
    }
    p->nprev = l->n;
    if (relayout) p->layout_frame = p->frame;

    // Static layer: only when the layout or labels changed (retried until it succeeds).
    if (p->static_layout != p->layout_frame || !p->stat[p->stat_cur]) {
        int si = pick_slot(p->stat, p->sctx, NULL, 2, pw, ph, scale);
        if (si >= 0 && IOSurfaceLock(p->stat[si], 0, NULL) == kIOReturnSuccess) {
            dc_t c = {p, {IOSurfaceGetBaseAddress(p->stat[si]), IOSurfaceGetBytesPerRow(p->stat[si]) / 4, pw, ph}, p->sctx[si], scale, scroll, dark};
            blit_clear(&c.t);
            for (int i = 0; i < l->n; i++) {
                float top = p->ypos[i], hgt = item_height(&l->item[i]);
                if (top + hgt - scroll < 0 || top - scroll > height) continue;
                draw_item(&c, &l->item[i], top, PART_STATIC);
            }
            CGContextFlush(c.cg);
            IOSurfaceUnlock(p->stat[si], 0, NULL);
            p->stat_cur = si;
            p->static_layout = p->layout_frame;
            p->static_repaints++;
            *static_out = (IOSurfaceRef)CFRetain(p->stat[si]);
        }
    }

    // Dynamic layer: rows changed since this surface was last shown.
    int pick = pick_slot(p->surf, p->ctx, p->surf_frame, 3, pw, ph, scale);
    if (pick < 0) return NULL;
    IOSurfaceRef sf = p->surf[pick];
    if (IOSurfaceLock(sf, 0, NULL) != kIOReturnSuccess) return NULL;
    dc_t c = {p, {IOSurfaceGetBaseAddress(sf), IOSurfaceGetBytesPerRow(sf) / 4, pw, ph}, p->ctx[pick], scale, scroll, dark};
    uint32_t since = p->surf_frame[pick];
    int full = since == 0 || p->layout_frame > since;
    if (full) {
        blit_clear(&c.t);
        memset(p->sp_valid[pick], 0, sizeof p->sp_valid[pick]);
        memset(p->ext[pick], 0, sizeof p->ext[pick]);
        p->full_repaints++;
    }
    p->last_scrolled = 0;
    p->last_copied = 0;
    // Copy-back: this surface is a frame or more behind the one on screen. Rows that
    // changed since it was painted, but not in this frame, are copied from the front
    // surface; rows changed in this frame are drawn below.
    int front = p->cur;
    uint32_t ff = p->surf_frame[front];
    if (!full && front != pick && p->surf[front] && ff > since && p->layout_frame <= ff &&
        IOSurfaceLock(p->surf[front], kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {
        const uint32_t *src = IOSurfaceGetBaseAddress(p->surf[front]);
        size_t sstride = IOSurfaceGetBytesPerRow(p->surf[front]) / 4;
        for (int i = 0; i < l->n; i++) {
            const pitem_t *it = &l->item[i];
            float top = p->ypos[i], hgt = item_height(it);
            if (top + hgt - scroll < 0 || top - scroll > height) continue;
            if (p->changed[i] <= since) continue;                         // already current here
            if (p->changed[i] > ff) continue;  // drawn (or scrolled from this surface's own state) below
            int y0 = dev_y_bottom(&c, top + hgt), y1 = dev_y_bottom(&c, top);
            if (it->kind == PI_SPARK && p->sp_valid[pick][i] && p->sp_valid[front][i]) {
                // Only the box rows that can hold ink in either surface differ.
                int by = dev_y_bottom(&c, top + 34);
                int rows = p->sp_inktop[pick][i] > p->sp_inktop[front][i] ? p->sp_inktop[pick][i] : p->sp_inktop[front][i];
                y0 = by;
                y1 = by + rows;
            }
            if (y0 < 0) y0 = 0;
            if (y1 > ph) y1 = ph;
            // Columns either surface may have inked for this item.
            int x0 = p->ext[pick][i][0] < p->ext[front][i][0] ? p->ext[pick][i][0] : p->ext[front][i][0];
            int x1 = p->ext[pick][i][1] > p->ext[front][i][1] ? p->ext[pick][i][1] : p->ext[front][i][1];
            if (p->ext[pick][i][1] <= p->ext[pick][i][0]) { x0 = p->ext[front][i][0]; x1 = p->ext[front][i][1]; }
            else if (p->ext[front][i][1] <= p->ext[front][i][0]) { x0 = p->ext[pick][i][0]; x1 = p->ext[pick][i][1]; }
            for (int y = y0; y < y1 && x1 > x0; y++)
                memcpy(c.t.buf + (size_t)(ph - 1 - y) * c.t.stride + x0, src + (size_t)(ph - 1 - y) * sstride + x0, (size_t)(x1 - x0) * 4);
            p->ext[pick][i][0] = p->ext[front][i][0];
            p->ext[pick][i][1] = p->ext[front][i][1];
            if (it->kind == PI_SPARK) {
                p->sp_serial[pick][i] = p->sp_serial[front][i];
                p->sp_scale[pick][i] = p->sp_scale[front][i];
                p->sp_valid[pick][i] = p->sp_valid[front][i];
                p->sp_inktop[pick][i] = p->sp_inktop[front][i];
            }
            p->last_copied++;
        }
        IOSurfaceUnlock(p->surf[front], kIOSurfaceLockReadOnly, NULL);
        since = ff;
    }
    for (int i = 0; i < l->n; i++) {
        float top = p->ypos[i], hgt = item_height(&l->item[i]);
        if (top + hgt - scroll < 0 || top - scroll > height) continue;
        const pitem_t *it = &l->item[i];
        if (!full && p->changed[i] <= since) continue;                 // unchanged since this surface was shown
        if (!full && it->kind == PI_SPARK && spark_scroll(p, &c, pick, i, it, top)) { (*painted)++; continue; }
        if (!full) clear_band(&c.t, dev_y_bottom(&c, top + hgt), dev_y_bottom(&c, top), p->ext[pick][i][0], p->ext[pick][i][1]);
        c.ix0 = pw;
        c.ix1 = 0;
        draw_item(&c, it, top, PART_DYNAMIC);
        if (!text_only(it->kind)) { c.ix0 = 0; c.ix1 = pw; }
        p->ext[pick][i][0] = (uint16_t)(c.ix1 > c.ix0 ? c.ix0 : 0);
        p->ext[pick][i][1] = (uint16_t)(c.ix1 > c.ix0 ? c.ix1 : 0);
        if (it->kind == PI_SPARK) {
            p->sp_serial[pick][i] = it->serial;
            p->sp_scale[pick][i] = it->vscale;
            p->sp_valid[pick][i] = 1;
            p->sp_inktop[pick][i] = (uint8_t)spark_inktop(it, (int)lroundf(34 * scale), scale);
        }
        (*painted)++;
    }
    CGContextFlush(c.cg);
    IOSurfaceUnlock(sf, 0, NULL);
    p->surf_frame[pick] = p->frame;
    p->cur = pick;
    return (IOSurfaceRef)CFRetain(sf);
}
