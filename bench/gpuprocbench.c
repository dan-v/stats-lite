// gpuprocbench: instructions and client reads per gpuprocs_sample() refresh,
// 2 s apart, with device utilization from PerformanceStatistics as the app passes it.
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../src/core/fastio.h"
#include "../src/core/procs.h"

static uint64_t instr(void) { struct rusage_info_v6 r; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r); return r.ri_instructions; }

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 15;
    io_service_t g = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    fio_key_t k[1] = {FIO_KEY("Device Utilization %")};
    uint64_t total = 0;
    for (int r = 0; r < rounds; r++) {
        int64_t u = -1;
        fio_read_numbers(g, "PerformanceStatistics", k, 1, &u);
        proc_row_t rows[5];
        int n;
        uint64_t i0 = instr();
        int ok = gpuprocs_sample(u >= 0 ? (float)u / 100 : -1, rows, &n, 5);
        uint64_t d = instr() - i0;
        if (r) total += d;
        printf("util %3lld%%  reads %3u  %7llu instr  ok %d |", u, gpuprocs_last_reads(), d, ok);
        for (int i = 0; i < n; i++) printf(" %s[%d] %.1f%%", rows[i].name, rows[i].pid, rows[i].cpu * 100);
        printf("\n");
        sleep(2);
    }
    printf("mean after the first: %llu instr/refresh\n", total / (rounds - 1));
}
