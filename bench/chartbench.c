// Strip with text + chart cells like style "both": shown surface held, CPU/GPU every frame, RAM every 2nd.
#include <libproc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/render.h"
static uint64_t instr(void) { struct rusage_info_v6 ru; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru); return ru.ri_instructions; }
static float series(int k, int which) { return which == 0 ? 0.25f + 0.2f * sinf(k * 0.31f) : which == 1 ? 0.06f + 0.02f * (k % 3) : 0.62f + 0.01f * (k % 2); }
static void fill(strip_t *st, int n) {
    memset(st, 0, sizeof *st);
    st->labels = 1; st->dark = 1; st->scale = 2; st->height = 24;
    st->accent[0] = 0.2f; st->accent[1] = 0.5f; st->accent[2] = 1;
    const char *labs[] = {"CPU", "GPU", "RAM", "SSD"};
    int cols = renderer_chart_cols(2), h = renderer_chart_height(2, 0);
    for (int m = 0; m < 4; m++) {
        cell_t *c = &st->cell[st->ncells++];
        strcpy(c->label, labs[m]);
        int t = m == 2 ? n / 2 : n;  // RAM advances every 2nd frame
        snprintf(c->top, sizeof c->top, "%d%%", (int)(series(t, m < 3 ? m : 2) * 100));
        if (m == 3) break;
        cell_t *ch = &st->cell[st->ncells++];
        ch->kind = CELL_CHART; ch->ncols = (uint8_t)cols;
        for (int i = 0; i < cols; i++) {
            int k = t - (cols - 1 - i);
            ch->col[i] = (uint8_t)lroundf(series(k, m) * h);
            if (m == 0) { ch->col2[i] = (uint8_t)lroundf(series(k, m) * 0.3f * h); }
        }
        if (m == 0) ch->stacked = 1;
    }
}
int main(void) {
    renderer_t r; renderer_init(&r); r.mode = RENDER_BLIT_SURFACE;
    strip_t st;
    int hold = !getenv("NOHOLD"), N = 3000;
    IOSurfaceRef shown = NULL;
    uint64_t i0 = 0;
    for (int n = -20; n < N; n++) {
        if (n == 0) i0 = instr();
        fill(&st, n + 100);
        CGFloat w = renderer_measure(&r, &st);
        IOSurfaceRef s = renderer_draw_surface(&r, &st, w);
        if (hold && s) IOSurfaceIncrementUseCount(s);
        if (shown) { if (hold) IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
        shown = s;
    }
    printf("%s: %llu instr/frame (%d cells)\n", hold ? "held  " : "nohold", (instr() - i0) / N, st.ncells);
    if (getenv("PNG")) {  // compare with a fresh full render
        renderer_t r2; renderer_init(&r2); r2.mode = RENDER_BLIT_SURFACE;
        IOSurfaceRef f = renderer_draw_surface(&r2, &st, renderer_measure(&r2, &st));
        IOSurfaceLock(shown, kIOSurfaceLockReadOnly, NULL); IOSurfaceLock(f, kIOSurfaceLockReadOnly, NULL);
        size_t bad = 0, rb = IOSurfaceGetWidth(f) * 4;
        for (size_t y = 0; y < IOSurfaceGetHeight(f); y++)
            bad += memcmp((uint8_t *)IOSurfaceGetBaseAddress(shown) + y * IOSurfaceGetBytesPerRow(shown), (uint8_t *)IOSurfaceGetBaseAddress(f) + y * IOSurfaceGetBytesPerRow(f), rb) != 0;
        printf("rows differing from a full render: %zu\n", bad);
    }
    return 0;
}
