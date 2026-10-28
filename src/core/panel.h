// panel: the detail panel, drawn with glyph atlases and pixel primitives into a
// pool of IOSurfaces. The app builds a display list of rows each tick; rows are
// hashed and each surface is repainted only where rows changed since it was shown.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdint.h>

#include "render.h"

enum {
    PI_HEADER,    // a: title, b: big value (right)
    PI_ROW,       // a: label (secondary, left), b: value (right)
    PI_SMALL,     // a: left (primary small), b: right (secondary small)
    PI_CAPTION,   // a: caption (secondary small)
    PI_SPARK,     // series 0 (and 1): fractions 0..1 of the box height, oldest first
    PI_DAY,       // 24 h: per-column avg/max heights (fractions), NaN-free; n columns
    PI_CORES,     // per-core bars: v[i] fraction, tag[i] cluster index (0 = first color)
    PI_BAR,       // single progress bar: v[0]
    PI_DIVIDER,
    PI_GAP,       // vertical space of `h` points
};

#define PANEL_MAX_ITEMS 96
#define PANEL_SERIES 160
// Samples a sparkline spans: 2 pt apart (272 pt / 136), so a new sample scrolls by
// whole device pixels at any scale.
#define HIST_CAP 137

typedef struct {
    uint8_t kind, nseries, n, color, color2;  // color: index into the panel palette
    float h;                                  // PI_GAP height
    uint32_t serial;                          // spark: total samples ever pushed (scroll detection)
    float vscale;                             // spark: normalization used (a change forces a redraw)
    char a[48], b[48];
    float v[2][PANEL_SERIES];
    uint8_t tag[PANEL_SERIES];
} pitem_t;

enum { PC_BLUE, PC_PURPLE, PC_GREEN, PC_TEAL, PC_PINK, PC_ORANGE, PC_RED, PC_ACCENT, PC_COUNT };

typedef struct {
    int n;
    pitem_t item[PANEL_MAX_ITEMS];
} plist_t;

typedef struct {
    glyphset_t title, value, label, small;
    colorlut_t lut_primary[2], lut_secondary[2];  // [dark]
    uint32_t palette[2][PC_COUNT];                // premultiplied, [dark]
    IOSurfaceRef surf[3];
    CGContextRef ctx[3];
    uint32_t surf_frame[3];  // frame number each surface was last painted at (0 = never)
    // static layer (labels, backgrounds, dividers): repainted only on layout changes
    IOSurfaceRef stat[2];
    CGContextRef sctx[2];
    int stat_cur;
    uint32_t static_layout;  // layout_frame the static layer was last painted for
    uint64_t shash[PANEL_MAX_ITEMS];
    int cur, pw, ph;
    float scale;
    uint8_t dark;
    // previous display list state (hash + y) and change frame per row
    uint32_t frame;
    int nprev;
    uint64_t hash[PANEL_MAX_ITEMS];
    float ypos[PANEL_MAX_ITEMS];
    uint32_t changed[PANEL_MAX_ITEMS];
    uint32_t layout_frame;  // last frame the layout (positions/size) changed
    // per dynamic surface: what each spark row showed when that surface was painted
    uint32_t sp_serial[3][PANEL_MAX_ITEMS];
    float sp_scale[3][PANEL_MAX_ITEMS];
    uint8_t sp_valid[3][PANEL_MAX_ITEMS];
    uint8_t sp_inktop[3][PANEL_MAX_ITEMS];  // rows above the box bottom that may hold ink
    uint16_t ext[3][PANEL_MAX_ITEMS][2];    // per surface: device columns [x0, x1) an item's dynamic ink may cover
    int last_scrolled;  // benchmarks: spark rows updated by scrolling in the last frame
    int last_copied;    // benchmarks: rows copied from the front surface in the last frame
    unsigned full_repaints, static_repaints;  // benchmarks: counters
    float scroll;
} panel_t;

#define PANEL_W 300.0f

void panel_init(panel_t *p);
// Content height in points for a list (layout only).
float panel_measure(const plist_t *l);
// Renders the list (scrolled by `scroll` points) as two PANEL_W x height layers.
// Returns the dynamic layer (values, chart ink; retained, NULL when no surface is
// free). *static_out is set (retained) only when the static layer was repainted.
// *painted_rows receives the number of dynamic rows repainted.
IOSurfaceRef panel_render(panel_t *p, const plist_t *l, float height, float scale, int dark, float scroll, int *painted_rows,
                          IOSurfaceRef *static_out);
void panel_release(panel_t *p);  // free surfaces when the panel closes

// Display-list builders.
void pl_reset(plist_t *l);
void pl_header(plist_t *l, const char *title, const char *value);
void pl_row(plist_t *l, const char *label, const char *value);
void pl_small(plist_t *l, const char *left, const char *right);
void pl_caption(plist_t *l, const char *text);
pitem_t *pl_spark(plist_t *l, int color, int color2);  // fill v[0..1][0..n), set n/nseries
pitem_t *pl_day(plist_t *l, int color);
pitem_t *pl_cores(plist_t *l);
void pl_bar(plist_t *l, float frac, int color);
void pl_divider(plist_t *l);
void pl_gap(plist_t *l, float h);
