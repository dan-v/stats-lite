// sparkbench: per-frame cost of one changing row of each kind in the panel renderer.
#include <libproc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include "../src/core/panel.h"
static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }
static panel_t p;
static plist_t l;
static void one(int kind, int t) {
    pl_reset(&l);
    char a[16];
    snprintf(a, sizeof a, "%d%%", t % 100);
    switch (kind) {
    case 0: { pitem_t *s = pl_spark(&l, PC_BLUE, 0); s->n = HIST_CAP; s->serial = (uint32_t)t + 1000; s->vscale = 1; for (int i = 0; i < HIST_CAP; i++) s->v[0][i] = 0.4f + 0.3f * sinf((i + t) * 0.2f); break; }
    case 1: pl_row(&l, "System", a); break;
    case 2: pl_header(&l, "CPU", a); break;
    case 3: { pitem_t *c = pl_cores(&l); c->n = 18; for (int i = 0; i < 18; i++) { c->v[0][i] = fabsf(sinf(i + t)); c->tag[i] = i > 11; } break; }
    case 4: { char b[32]; snprintf(b, sizeof b, "%.1f%%", t * 0.1); pl_small(&l, "WindowServer", b); break; }
    }
}
#ifdef PANEL_PROFILE
extern uint64_t g_pp[4];
#endif
int main(void) {
    const char *names[] = {"spark (137 samples)", "row (label+value)", "header", "cores (18)", "process row"};
    panel_init(&p);
    for (int k = 0; k < (getenv("ONLY_SPARK") ? 1 : 5); k++) {
        panel_release(&p);
        one(k, 0);
        int painted;
        IOSurfaceRef st = NULL, s = panel_render(&p, &l, panel_measure(&l), 2, 1, 0, &painted, &st);
        if (s) CFRelease(s);
        if (st) CFRelease(st);
        const int N = getenv("N") ? atoi(getenv("N")) : 2000;
        uint64_t i0 = instr();
        for (int t = 1; t <= N; t++) {
            one(k, t);
            IOSurfaceRef st2 = NULL, s2 = panel_render(&p, &l, panel_measure(&l), 2, 1, 0, &painted, &st2);
            if (s2) CFRelease(s2);
            if (st2) CFRelease(st2);
        }
        printf("%-22s %8.0f instr/frame (incl. ~fixed frame overhead)\n", names[k], (double)(instr() - i0) / N);
#ifdef PANEL_PROFILE
        if (k == 0) printf("   scroll memmove %.0f | right columns %.0f | left corner %.0f (per frame, incl. ~8k counter overhead each)\n", g_pp[0] / (double)N, g_pp[1] / (double)N, g_pp[2] / (double)N);
#endif
    }
    pl_reset(&l);
    pl_gap(&l, 30);
    uint64_t i0 = instr();
    int painted;
    for (int t = 0; t < 2000; t++) { IOSurfaceRef st = NULL, s = panel_render(&p, &l, 40, 2, 1, 0, &painted, &st); if (s) CFRelease(s); if (st) CFRelease(st); }
    printf("%-22s %8.0f instr/frame\n", "empty frame overhead", (double)(instr() - i0) / 2000);
    return 0;
}
