// blit: text and shapes composited into a BGRA buffer without CoreGraphics.
// Glyphs are rasterized once per scale into coverage masks; a per-color 256-entry
// table maps coverage to the premultiplied pixel, so only overlapping pixels blend.
#pragma once
#include <CoreText/CoreText.h>
#include <stdint.h>

typedef struct {
    int16_t x0, y0;   // mask origin relative to the pen, device pixels, y up
    uint16_t w, h;
    uint16_t stride;  // row stride: w rounded up to 16 (zero coverage padding, so SIMD has no tails)
    uint32_t off;     // offset into atlas pixels
} gmask_t;

typedef struct {
    float scale;
    int dark_ink;     // masks rasterized with dark (black) ink: CG dilates light text more
    gmask_t m[128];
    float adv[128];   // advance in device pixels
    uint8_t *pix;
    size_t used, cap;
} atlas_t;

typedef struct {
    uint32_t *buf;     // premultiplied BGRA (kCGImageAlphaPremultipliedFirst | ByteOrder32Little)
    size_t stride;     // in pixels
    int pw, ph;
} target_t;

typedef struct { uint32_t px[256]; } colorlut_t;

void atlas_build(atlas_t *a, CTFontRef font, const CGGlyph glyph[128], float scale, int dark_ink);
void atlas_free(atlas_t *a);
void colorlut_init(colorlut_t *l, float r, float g, float b, float alpha);
// Draws s with its pen starting at x (device px, may be fractional) and the
// baseline at y (device px from the bottom edge).
void blit_text(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c);
void blit_clear(const target_t *t);
#if defined(__aarch64__)
void blit_text_neon(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c);
void blit_text_asm(const target_t *t, const atlas_t *a, const char *s, float x, int y, const colorlut_t *c);  // blit_arm64.S
// All variants are pixel-identical to the scalar blit_text.
#define BLIT_TEXT blit_text_asm
#else
#define BLIT_TEXT blit_text
#endif

// ---- shapes (device pixels, y measured from the bottom edge) ----------------------
// Fills a rectangle with a premultiplied color (src-over).
void blit_fill(const target_t *t, int x, int y, int w, int h, uint32_t premul);
// Column chart: one column per sample (oldest first), heights 0..h device pixels,
// filled with `fill` under a 1-pixel `line`. mirrored: grows down from the top edge.
void blit_columns(const target_t *t, int x, int y, int h, const uint8_t *heights, int n, uint32_t fill, uint32_t line,
                  int mirrored);
void blit_columns_range(const target_t *t, int x, int y, int h, const uint8_t *heights, int i0, int i1, uint32_t fill,
                        uint32_t line, int mirrored);
uint32_t premul_rgba(float r, float g, float b, float a);

uint32_t blit_blend(uint32_t src, uint32_t dst);  // premultiplied src-over
// Blends `premul` scaled by coverage (0..1) into one pixel (y from the bottom).
void blit_put_cov(const target_t *t, int x, int y, uint32_t premul, float coverage);
// Filled rounded rectangle with anti-aliased corners (device pixels, y from bottom).
void blit_round_rect(const target_t *t, int x, int y, int w, int h, float radius, uint32_t premul);
