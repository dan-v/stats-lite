// Realistic strip frame: shown surface held, CPU and GPU change every frame, RAM every 2nd.
#include <libproc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/render.h"
static uint64_t instr(void) { struct rusage_info_v6 ru; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru); return ru.ri_instructions; }
int main(void) {
    renderer_t r; renderer_init(&r); r.mode = RENDER_BLIT_SURFACE;
    strip_t st; memset(&st, 0, sizeof st);
    st.ncells = 4; st.labels = 1; st.dark = 1; st.scale = 2; st.height = getenv("H") ? atof(getenv("H")) : 24;
    const char *labs[] = {"CPU", "GPU", "RAM", "SSD"};
    for (int i = 0; i < 4; i++) { strcpy(st.cell[i].label, labs[i]); strcpy(st.cell[i].top, "42%"); }
    int hold = !getenv("NOHOLD"), N = getenv("N") ? atoi(getenv("N")) : 3000;
    IOSurfaceRef shown = NULL;
    uint64_t i0 = 0;
    for (int n = -20; n < N; n++) {
        if (n == 0) i0 = instr();
        snprintf(st.cell[0].top, sizeof st.cell[0].top, "%d%%", n % 13 + 3);
        snprintf(st.cell[1].top, sizeof st.cell[1].top, "%d%%", n % 3 + 5);
        snprintf(st.cell[2].top, sizeof st.cell[2].top, "%d%%", 60 + (n / 2) % 3);
        CGFloat w = renderer_measure(&r, &st);
        IOSurfaceRef s = renderer_draw_surface(&r, &st, w);
        if (hold && s) IOSurfaceIncrementUseCount(s);
        if (shown) { if (hold) IOSurfaceDecrementUseCount(shown); CFRelease(shown); }
        shown = s;
    }
    printf("%s: %llu instr/frame\n", hold ? "held  " : "nohold", (instr() - i0) / N);
#ifdef RP
    extern uint64_t g_rp[8];
    const char *nm[] = {"pick", "lock", "dirty", "paint", "bookkeep", "cgflush", "unlock"};
    for (int i = 0; i < 7; i++) printf("  %-9s %6llu\n", nm[i], g_rp[i] / g_rp[7]);
#endif
    return 0;
}
