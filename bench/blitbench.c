// blitbench: scalar (LUT) vs NEON glyph compositing — instructions, time, and
// pixel-exactness on a menu-bar-sized target.
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/render.h"
static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }
int main(void) {
    renderer_t r;
    renderer_init(&r);
    glyphset_t *g = &r.value;
    atlas_build(&g->atlas[0], g->font, g->glyph, 2, 0);
    static uint32_t bufA[600 * 48], bufB[600 * 48], bufC[600 * 48];
    target_t ta = {bufA, 600, 600, 48}, tb = {bufB, 600, 600, 48}, tc = {bufC, 600, 600, 48};
    const char *strs[] = {"42%", "100%", "7%", "66%", "13:37"};
    mach_timebase_info_data_t tbi; mach_timebase_info(&tbi);
    const int N = 20000;
    const char *names[] = {"scalar", "NEON", "asm"};
    for (int mode = 0; mode < 3; mode++) {
        target_t *t = mode == 2 ? &tc : mode ? &tb : &ta;
        uint64_t i0 = instr(), t0 = mach_absolute_time();
        for (int n = 0; n < N; n++) {
            memset(t->buf, 0, 600 * 48 * 4);
            for (int k = 0; k < 5; k++) {
                if (mode == 2) blit_text_asm(t, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
                else if (mode) blit_text_neon(t, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
                else blit_text(t, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
            }
        }
        uint64_t t1 = mach_absolute_time(), i1 = instr();
        printf("%-7s %7.0f instr/frame  %6.2f us/frame  (5 strings + 115 KB clear)\n", names[mode],
               (double)(i1 - i0) / N, (double)(t1 - t0) * tbi.numer / tbi.denom / N / 1000);
    }
    // exactness: render once more over a non-empty background too
    for (int k = 0; k < 600 * 48; k++) bufA[k] = bufB[k] = bufC[k] = (k % 7) ? 0 : 0x80402010u;
    for (int k = 0; k < 5; k++) {
        blit_text(&ta, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
        blit_text_neon(&tb, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
        blit_text_asm(&tc, &g->atlas[0], strs[k], 10 + k * 110, 10, &r.lut_dark);
    }
    int maxd = 0, maxe = 0;
    for (int k = 0; k < 600 * 48 * 4; k++) {
        int dd = abs(((uint8_t *)bufA)[k] - ((uint8_t *)bufB)[k]), de = abs(((uint8_t *)bufA)[k] - ((uint8_t *)bufC)[k]);
        if (dd > maxd) maxd = dd;
        if (de > maxe) maxe = de;
    }
    printf("max channel difference vs scalar: NEON %d, asm %d\n", maxd, maxe);
    return 0;
}
