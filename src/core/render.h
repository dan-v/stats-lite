// render: draws every enabled menu bar module into one bitmap using pre-resolved
// glyphs (no text layout per frame).
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <IOSurface/IOSurfaceRef.h>

#include "blit.h"
#include <stdint.h>

// Special characters available in strings drawn by the glyph cache.
#define CH_DEGREE '\x01'
#define CH_UP '\x02'
#define CH_DOWN '\x03'

typedef struct {
    CTFontRef font;
    CGGlyph glyph[128];
    CGFloat adv[128];
    CGFloat ascent, descent;
    atlas_t atlas[2];  // pre-rasterized coverage masks (blit modes): [0] light ink, [1] dark ink
} glyphset_t;

void glyphset_init(glyphset_t *g, CTFontRef font);  // retains font
CGFloat glyphset_width(const glyphset_t *g, const char *s);
// Draws s with its left edge at x and baseline at y (points, bottom-left origin).
void glyphset_draw(CGContextRef ctx, const glyphset_t *g, const char *s, CGFloat x, CGFloat y);

#define STRIP_MAX_CELLS 16  // 7 modules x (text + chart), with room to spare

enum { CELL_MINI = 0, CELL_SPEED = 1, CELL_BATTERY = 2, CELL_CHART = 3, CELL_NETCHART = 4 };

#define CHART_W 28.0f   // chart box width in points
#define CHART_COLS 64   // max columns (device pixels) a chart can hold

typedef struct {
    uint8_t kind;
    uint8_t warn;        // 1: draw value in the warning color (e.g. memory pressure)
    uint8_t charging;    // battery
    uint8_t level;       // battery 0..100
    char label[8];       // mini
    uint8_t wide;        // width override in points (0: the kind's default)
    char top[12];        // mini value, speed upload
    char bottom[12];     // speed download
    uint8_t ncols;               // chart: columns in use (device pixels)
    uint8_t stacked;             // chart: col2 is a darker lower band (e.g. CPU system under user)
    uint8_t spark;               // mini: sparkline (col, ncols) in the label row, right of the label
    uint8_t col[CHART_COLS];     // chart heights in device pixels, oldest first (upload for net)
    uint8_t col2[CHART_COLS];    // net chart: download heights; stacked chart: lower band
} cell_t;

typedef struct {
    int ncells;
    cell_t cell[STRIP_MAX_CELLS];
    uint8_t dark;
    uint8_t labels;
    float scale;
    float height;  // points
    float accent[3];  // chart color (system accent), sRGB
    uint8_t mono;     // monochrome charts (menu bar text color) instead of the accent
} strip_t;

// paint: CoreGraphics text (0-2) or glyph-atlas blitter (3-4); export: CGImage or IOSurface.
#define SURF_POOL 3

enum { RENDER_IMAGE_CROP = 0, RENDER_IMAGE_EXACT = 1, RENDER_SURFACE = 2, RENDER_BLIT_IMAGE = 3, RENDER_BLIT_SURFACE = 4 };

typedef struct {
    glyphset_t label, value, small;
    int mode;  // RENDER_*
    CGContextRef ctx;
    void *pixels;
    int pw, ph;  // pixel size of ctx
    float scale;
    IOSurfaceRef surf[3];
    CGContextRef sctx[3];
    int cur, sw, sh;
    float sscale;
    colorlut_t lut_dark, lut_light, lut_warn;
    float scale_for_atlas;
    CGColorRef cg_fg[2], cg_warn;  // cached CG colors (CoreGraphics text and battery shapes)
    strip_t surf_strip[3];         // what each pooled surface currently shows (partial repaints)
    uint8_t surf_valid[3];
    uint32_t surf_painted[3], paint_count;  // recency of each pooled surface
    unsigned charts_scrolled;               // benchmarks/tests: chart cells moved instead of redrawn
    unsigned values_only;                   // benchmarks/tests: percentage cells repainted without their label
    const target_t *cur_target;  // pixel view of the frame being painted
    int dark_for_atlas;
} renderer_t;

void renderer_init(renderer_t *r);
// Chart geometry in device pixels for a given scale (used to quantize history into cell_t.col).
int renderer_spark_cols(float scale);    // sparkline columns (device pixels) in a mini cell
int renderer_spark_height(float scale);  // sparkline height in device pixels
int renderer_chart_cols(float scale);
int renderer_chart_height(float scale, int half);
// Width in points that the strip needs.
CGFloat renderer_measure(const renderer_t *r, const strip_t *s);
// Renders the strip; returns a new CGImage (caller releases) or NULL.
CGImageRef renderer_draw(renderer_t *r, const strip_t *s, CGFloat width);
// RENDER_SURFACE / RENDER_BLIT_SURFACE: renders into the back of two IOSurfaces; returns it retained.
IOSurfaceRef renderer_draw_surface(renderer_t *r, const strip_t *s, CGFloat width);
