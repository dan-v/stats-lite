#include "blit.h"

#include <math.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <stdlib.h>
#include <string.h>

void atlas_free(atlas_t *a) {
    free(a->pix);
    memset(a, 0, sizeof(*a));
}

void atlas_build(atlas_t *a, CTFontRef font, const CGGlyph glyph[128], float scale, int dark_ink) {
    atlas_free(a);
    a->scale = scale;
    a->dark_ink = dark_ink;
    CGRect box[128];
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, glyph, box, 128);
    CGSize adv[128];
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, glyph, adv, 128);
    for (int i = 0; i < 128; i++) {
        a->adv[i] = (float)(adv[i].width * scale);
        if (CGRectIsEmpty(box[i])) continue;  // space
        int x0 = (int)floor(box[i].origin.x * scale) - 1, y0 = (int)floor(box[i].origin.y * scale) - 1;
        int x1 = (int)ceil((box[i].origin.x + box[i].size.width) * scale) + 1;
        int y1 = (int)ceil((box[i].origin.y + box[i].size.height) * scale) + 1;
        uint16_t w = (uint16_t)(x1 - x0), st = (uint16_t)((w + 15) & ~15);
        a->m[i] = (gmask_t){(int16_t)x0, (int16_t)y0, w, (uint16_t)(y1 - y0), st, 0};
        a->cap += (size_t)st * (size_t)(y1 - y0);
    }
    a->pix = calloc(a->cap ? a->cap : 1, 1);
    for (int i = 0; i < 128; i++) {
        gmask_t *m = &a->m[i];
        if (!m->w) continue;
        m->off = (uint32_t)a->used;
        a->used += (size_t)m->stride * m->h;
        // Rasterize exactly as the CoreGraphics path does (same ink, same font
        // smoothing) and keep the alpha channel as the coverage mask.
        uint32_t *tmp = calloc((size_t)m->w * m->h, 4);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef c = CGBitmapContextCreate(tmp, m->w, m->h, 8, (size_t)m->w * 4, cs,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
        CGContextSetRGBFillColor(c, dark_ink ? 0 : 1, dark_ink ? 0 : 1, dark_ink ? 0 : 1, 1);
        CGContextTranslateCTM(c, -m->x0, -m->y0);
        CGContextScaleCTM(c, scale, scale);
        CGContextSetTextMatrix(c, CGAffineTransformIdentity);
        CGPoint p = {0, 0};
        CTFontDrawGlyphs(font, &glyph[i], &p, 1, c);
        CGContextRelease(c);
        uint8_t *dst = a->pix + m->off;
        for (size_t r = 0; r < m->h; r++)
            for (size_t k = 0; k < m->w; k++) dst[r * m->stride + k] = (uint8_t)(tmp[r * m->w + k] >> 24);
        free(tmp);
    }
}

void colorlut_init(colorlut_t *l, float r, float g, float b, float alpha) {
    for (int c = 0; c < 256; c++) {
        float a = c / 255.0f * alpha;
        uint32_t A = (uint32_t)lroundf(a * 255), R = (uint32_t)lroundf(r * a * 255), G = (uint32_t)lroundf(g * a * 255),
                 B = (uint32_t)lroundf(b * a * 255);
        l->px[c] = A << 24 | R << 16 | G << 8 | B;
    }
}

static inline uint32_t mul255(uint32_t x, uint32_t y) {  // round(x*y/255)
    uint32_t t = x * y + 128;
    return (t + (t >> 8)) >> 8;
}

static inline uint32_t blend(uint32_t src, uint32_t dst) {  // premultiplied src over dst
    uint32_t ia = 255 - (src >> 24);
    uint32_t a = (src >> 24) + mul255(dst >> 24, ia);
    uint32_t r = ((src >> 16) & 255) + mul255((dst >> 16) & 255, ia);
    uint32_t g = ((src >> 8) & 255) + mul255((dst >> 8) & 255, ia);
    uint32_t b = (src & 255) + mul255(dst & 255, ia);
    return a << 24 | r << 16 | g << 8 | b;
}

void blit_text(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s & 127;
        const gmask_t *m = &a->m[ch];
        if (m->w) {
            int gx = (int)floorf(x + 0.5f) + m->x0;  // snap the pen to whole device pixels
            int top = t->ph - (y + m->y0 + m->h);    // buffer row of the mask's first row
            const uint8_t *src = a->pix + m->off;
            for (int row = 0; row < m->h; row++) {
                int by = top + row;
                if (by < 0 || by >= t->ph) continue;
                uint32_t *d = t->buf + (size_t)by * t->stride;
                const uint8_t *mrow = src + (size_t)row * m->stride;
                for (int col = 0; col < m->w; col++) {
                    int bx = gx + col;
                    uint8_t cov = mrow[col];
                    if (!cov || bx < 0 || bx >= t->pw) continue;
                    uint32_t px = c->px[cov];
                    d[bx] = d[bx] ? blend(px, d[bx]) : px;
                }
            }
        }
        x += a->adv[ch];
    }
}

void blit_clear(const target_t *t) {
    if (t->stride == (size_t)t->pw) memset(t->buf, 0, (size_t)t->pw * t->ph * 4);
    else
        for (int y = 0; y < t->ph; y++) memset(t->buf + (size_t)y * t->stride, 0, (size_t)t->pw * 4);
}

uint32_t premul_rgba(float r, float g, float b, float a) {
    uint32_t A = (uint32_t)lroundf(a * 255), R = (uint32_t)lroundf(r * a * 255), G = (uint32_t)lroundf(g * a * 255),
             B = (uint32_t)lroundf(b * a * 255);
    return A << 24 | R << 16 | G << 8 | B;
}

static inline void put(const target_t *t, int x, int row, uint32_t c) {
    uint32_t *d = &t->buf[(size_t)row * t->stride + (size_t)x];
    *d = *d ? blend(c, *d) : c;
}

void blit_fill(const target_t *t, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y; yy < y + h; yy++) {
        int row = t->ph - 1 - yy;
        if (row < 0 || row >= t->ph) continue;
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < t->pw) put(t, xx, row, c);
    }
}

void blit_columns(const target_t *t, int x, int y, int h, const uint8_t *heights, int n, uint32_t fill, uint32_t line,
                  int mirrored) {
    blit_columns_range(t, x, y, h, heights, 0, n, fill, line, mirrored);
}

// Columns [i0, i1) only, exactly as a full blit_columns call draws them (column i0's
// connector comes from heights[i0 - 1]).
void blit_columns_range(const target_t *t, int x, int y, int h, const uint8_t *heights, int i0, int i1, uint32_t fill,
                        uint32_t line, int mirrored) {
    int prev = i0 > 0 ? (heights[i0 - 1] > h ? h : heights[i0 - 1]) : -1;
    for (int i = i0; i < i1; i++) {
        int px = x + i;
        int v = heights[i] > h ? h : heights[i];
        // Continuous line: a vertical connector from the previous column's top.
        // A zero sample still draws the baseline, so idle reads as a flat line.
        int top = v > 0 ? v - 1 : 0;
        int lo = top, hi = top;
        if (prev >= 0) {
            int pt = prev > 0 ? prev - 1 : 0;
            if (pt < lo) lo = pt;
            if (pt > hi) hi = pt;
        }
        prev = v;
        if (px < 0 || px >= t->pw) continue;
        for (int k = 0; k <= hi && k < h; k++) {
            int is_line = k >= lo;
            if (!is_line && k >= v) continue;
            int yy = mirrored ? y + h - 1 - k : y + k;
            int row = t->ph - 1 - yy;
            if (row < 0 || row >= t->ph) continue;
            put(t, px, row, is_line ? line : fill);
        }
    }
}

uint32_t blit_blend(uint32_t src, uint32_t dst) { return blend(src, dst); }

static inline uint32_t scale_px(uint32_t c, uint32_t a8) {  // premultiplied pixel * a8/255
    return mul255(c >> 24, a8) << 24 | mul255((c >> 16) & 255, a8) << 16 | mul255((c >> 8) & 255, a8) << 8 | mul255(c & 255, a8);
}

void blit_put_cov(const target_t *t, int x, int y, uint32_t c, float cov) {
    if (x < 0 || x >= t->pw || y < 0 || y >= t->ph || cov <= 0) return;
    if (cov < 1) c = scale_px(c, (uint32_t)(cov * 255 + 0.5f));
    put(t, x, t->ph - 1 - y, c);
}

static inline void span(uint32_t *row, int x0, int x1, uint32_t c) {
    if (x1 <= x0) return;
    if ((c >> 24) == 255) { memset_pattern4(row + x0, &c, (size_t)(x1 - x0) * 4); return; }
    for (int i = x0; i < x1; i++) { uint32_t d = row[i]; row[i] = d ? blend(c, d) : c; }
}

static inline void corner_px(uint32_t *row, int px, float fx, float fy, float r, int w, int h, uint32_t c) {
    float cx = fx < r ? r : fx > w - r ? w - r : fx, cy = fy < r ? r : fy > h - r ? h - r : fy;
    float cov = 1;
    if (cx != fx && cy != fy) {
        float d = sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
        cov = r + 0.5f - d;
        if (cov <= 0) return;
        if (cov > 1) cov = 1;
    }
    uint32_t v = cov < 1 ? scale_px(c, (uint32_t)(cov * 255 + 0.5f)) : c;
    row[px] = row[px] ? blend(v, row[px]) : v;
}

void blit_round_rect(const target_t *t, int x, int y, int w, int h, float r, uint32_t c) {
    if (r > w / 2.0f) r = w / 2.0f;
    if (r > h / 2.0f) r = h / 2.0f;
    int ri = (int)ceilf(r);
    int xs = x < 0 ? 0 : x, xe = x + w > t->pw ? t->pw : x + w;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= t->ph) continue;
        uint32_t *row = t->buf + (size_t)(t->ph - 1 - dy) * t->stride;
        if (yy >= ri && yy < h - ri) { span(row, xs, xe, c); continue; }  // straight edge rows
        float fy = yy + 0.5f;
        int l = x + ri < xe ? x + ri : xe, rr = x + w - ri > xs ? x + w - ri : xs;
        for (int px = xs; px < l; px++) corner_px(row, px, px - x + 0.5f, fy, r, w, h, c);
        span(row, l > xs ? l : xs, rr < xe ? rr : xe, c);
        for (int px = rr > l ? rr : l; px < xe; px++) corner_px(row, px, px - x + 0.5f, fy, r, w, h, c);
    }
}

#if defined(__aarch64__)
// NEON blit_text: 16 pixels per iteration, d = s + d * (255 - sa) / 255 with the
// same rounding as the scalar path.
static inline uint8x16_t mul255_u8(uint8x16_t a, uint8x16_t b) {  // round(a*b/255) per lane
    uint16x8_t lo = vmull_u8(vget_low_u8(a), vget_low_u8(b)), hi = vmull_u8(vget_high_u8(a), vget_high_u8(b));
    lo = vaddq_u16(lo, vdupq_n_u16(128));
    hi = vaddq_u16(hi, vdupq_n_u16(128));
    lo = vaddq_u16(lo, vshrq_n_u16(lo, 8));
    hi = vaddq_u16(hi, vshrq_n_u16(hi, 8));
    return vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8));
}

void blit_text_neon(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c) {
    // full-coverage source pixel components (premultiplied) from the LUT's last entry
    uint32_t full = c->px[255];
    uint8x16_t cb = vdupq_n_u8((uint8_t)full), cg = vdupq_n_u8((uint8_t)(full >> 8)), cr = vdupq_n_u8((uint8_t)(full >> 16)),
               ca = vdupq_n_u8((uint8_t)(full >> 24)), k255 = vdupq_n_u8(255);
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s & 127;
        const gmask_t *m = &a->m[ch];
        if (m->w) {
            int gx = (int)floorf(x + 0.5f) + m->x0;
            int top = t->ph - (y + m->y0 + m->h);
            const uint8_t *src = a->pix + m->off;
            // Unclipped glyphs use the zero-padded stride: whole 16-pixel blocks, no tail.
            int padded = gx >= 0 && gx + m->stride <= t->pw;
            int x0 = gx < 0 ? 0 : gx, x1 = padded ? gx + m->stride : (gx + m->w > t->pw ? t->pw : gx + m->w);
            for (int row = 0; row < m->h; row++) {
                int by = top + row;
                if (by < 0 || by >= t->ph || x1 <= x0) continue;
                uint32_t *d = t->buf + (size_t)by * t->stride;
                const uint8_t *mrow = src + (size_t)row * m->stride + (x0 - gx);
                int n = x1 - x0, i = 0;
                for (; i + 16 <= n; i += 16) {
                    uint8x16_t cov = vld1q_u8(mrow + i);
                    if (vmaxvq_u8(cov) == 0) continue;  // empty run of the glyph mask
                    uint8x16x4_t px = vld4q_u8((const uint8_t *)(d + x0 + i));  // b, g, r, a planes
                    uint8x16_t sa = mul255_u8(ca, cov), inv = vsubq_u8(k255, sa);
                    px.val[0] = vaddq_u8(mul255_u8(cb, cov), mul255_u8(px.val[0], inv));
                    px.val[1] = vaddq_u8(mul255_u8(cg, cov), mul255_u8(px.val[1], inv));
                    px.val[2] = vaddq_u8(mul255_u8(cr, cov), mul255_u8(px.val[2], inv));
                    px.val[3] = vaddq_u8(sa, mul255_u8(px.val[3], inv));
                    vst4q_u8((uint8_t *)(d + x0 + i), px);
                }
                for (; i < n; i++) {  // tail: scalar, same math
                    uint8_t cv = mrow[i];
                    if (!cv) continue;
                    uint32_t sp = c->px[cv], *dp = &d[x0 + i];
                    *dp = *dp ? blend(sp, *dp) : sp;
                }
            }
        }
        x += a->adv[ch];
    }
}
#endif

#if defined(__aarch64__)
// Same compositor with the whole glyph in blit_arm64.S; only placement stays in C.

extern void blit_glyph_asm(uint32_t *dst, size_t dst_stride_bytes, const uint8_t *cov, size_t cov_stride, size_t blocks, size_t rows,
                           const uint8_t color[4]);

void blit_text_asm(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c) {
    uint32_t full = c->px[255];
    const uint8_t color[4] = {(uint8_t)full, (uint8_t)(full >> 8), (uint8_t)(full >> 16), (uint8_t)(full >> 24)};
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s & 127;
        const gmask_t *m = &a->m[ch];
        if (m->w) {
            int gx = (int)floorf(x + 0.5f) + m->x0;
            int top = t->ph - (y + m->y0 + m->h);
            if (gx >= 0 && gx + m->stride <= t->pw) {  // whole glyph in one assembly call
                int r0 = top < 0 ? -top : 0, r1 = top + m->h > t->ph ? t->ph - top : m->h;
                if (r1 > r0)
                    blit_glyph_asm(t->buf + (size_t)(top + r0) * t->stride + gx, t->stride * 4, a->pix + m->off + (size_t)r0 * m->stride,
                                   m->stride, m->stride / 16, (size_t)(r1 - r0), color);
            } else {  // clipped at the left/right edge: the C path
                char one[2] = {(char)ch, 0};
                blit_text_neon(t, a, one, x, y, c);
            }
        }
        x += a->adv[ch];
    }
}
#endif
