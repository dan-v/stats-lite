// renderbench: instructions and time per rendered menu bar frame, per render mode,
// for the four-widget strip at 2x. Values change every frame like a live CPU %.
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../src/core/render.h"

static uint64_t instr(void) {
    struct rusage_info_v6 ru;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru);
    return ru.ri_instructions;
}

int main(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const char *names[] = {"CG text + CGImage (crop)", "CG text + CGImage (exact)", "CG text + IOSurface",
                           "atlas blit + CGImage (exact)", "atlas blit + IOSurface"};
    for (int mode = 0; mode <= 4; mode++) {
        renderer_t r;
        renderer_init(&r);
        r.mode = mode;
        strip_t st;
        memset(&st, 0, sizeof st);
        st.ncells = 4; st.labels = 1; st.dark = 1; st.scale = 2; st.height = 24;
        const char *labs[] = {"CPU", "GPU", "RAM", "SSD"};
        for (int i = 0; i < 4; i++) { strcpy(st.cell[i].label, labs[i]); strcpy(st.cell[i].top, "42%"); }
        CGFloat w = renderer_measure(&r, &st);
        const int N = 3000;
        for (int warm = 0; warm < 20; warm++) {
            CFTypeRef c = mode == 2 || mode == 4 ? (CFTypeRef)renderer_draw_surface(&r, &st, w) : (CFTypeRef)renderer_draw(&r, &st, w);
            CFRelease(c);
        }
        uint64_t i0 = instr(), t0 = mach_absolute_time();
        for (int n = 0; n < N; n++) {
            snprintf(st.cell[0].top, sizeof st.cell[0].top, "%d%%", n % 100);  // value changes every frame
            CFTypeRef c = mode == 2 || mode == 4 ? (CFTypeRef)renderer_draw_surface(&r, &st, w) : (CFTypeRef)renderer_draw(&r, &st, w);
            CFRelease(c);
        }
        uint64_t t1 = mach_absolute_time(), i1 = instr();
        printf("%-30s %8.0f instr/frame %7.1f us/frame\n", names[mode], (double)(i1 - i0) / N,
               (double)(t1 - t0) * tb.numer / tb.denom / N / 1000.0);
    }
    return 0;
}
