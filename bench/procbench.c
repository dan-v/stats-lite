// procbench: instructions per top-process refresh for different full-scan periods.
#include <libproc.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/procs.h"
static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }
int main(void) {
    int periods[] = {0, 0, 0, 0, 0};
    int cold[] = {12, 12, 12, 12, 12};
    int warm[] = {3, 6, 3, 6, 6};
    float hot[] = {0.005f, 0.005f, 0.01f, 0.01f, 0.02f};
    for (int k = 0; k < 5; k++) {
        procs_reset();
        procs_set_full_every(periods[k]);
        if (cold[k]) procs_set_cold_period(cold[k]);
        procs_set_warm(warm[k], hot[k]);
        proc_row_t c[5], m[5];
        int nc, nm;
        for (int i = 0; i < 3; i++) { procs_sample(1, c, &nc, m, &nm, 5); usleep(200000); }  // warm: two full scans
        const int N = 30;
        uint64_t total = 0, reads = 0;
        for (int i = 0; i < N; i++) {
            uint64_t a = instr();
            procs_sample(1, c, &nc, m, &nm, 5);
            total += instr() - a;
            reads += procs_last_reads();
            usleep(250000);
        }
        if (periods[k]) printf("full scan every %d:          ", periods[k]); else printf("tiered cold %d warm %d hot>=%.1f%%:", cold[k], warm[k], hot[k] * 100);
        printf(" %4.0f reads %8.0f instr/refresh   top: %s %.1f%%, %s %.1f%%\n", (double)reads / N, (double)total / N,
               c[0].name, c[0].cpu * 100, c[1].name, c[1].cpu * 100);
    }
    return 0;
}
