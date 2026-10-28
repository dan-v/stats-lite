// rumeter: per-process resource deltas (proc_pid_rusage V6 + PROC_PIDTASKINFO) over
// a window. Retired instructions and energy (nJ) are far less noisy than %CPU.
//   rumeter <pid>[,<pid>...] <seconds>          measure one or more processes
//   rumeter --selftest                          verify time units with a busy loop
//
// Output: one JSON object per pid on stdout.

#include <libproc.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    struct rusage_info_v6 ru;
    struct proc_taskinfo ti;
    int ok_ru, ok_ti;
} snap_t;

static mach_timebase_info_data_t tb;

static double ticks_to_ms(uint64_t t) { return (double)t * tb.numer / tb.denom / 1e6; }

static void take(pid_t pid, snap_t *s) {
    memset(s, 0, sizeof(*s));
    s->ok_ru = proc_pid_rusage(pid, RUSAGE_INFO_V6, (rusage_info_t *)&s->ru) == 0;
    s->ok_ti = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &s->ti, sizeof(s->ti)) == (int)sizeof(s->ti);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int selftest(void) {
    snap_t a, b;
    take(getpid(), &a);
    double t0 = now_s();
    volatile uint64_t x = 0;
    while (now_s() - t0 < 1.0) x++;
    take(getpid(), &b);
    double wall = (now_s() - t0) * 1000;
    printf("wall=%.1fms user=%.1fms (timebase %u/%u) instr=%llu\n", wall,
           ticks_to_ms(b.ru.ri_user_time - a.ru.ri_user_time), tb.numer, tb.denom,
           (unsigned long long)(b.ru.ri_instructions - a.ru.ri_instructions));
    return 0;
}

int main(int argc, char **argv) {
    mach_timebase_info(&tb);
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc < 3) {
        fprintf(stderr, "usage: rumeter <pid>[,<pid>...] <seconds>\n");
        return 2;
    }

    pid_t pids[16];
    int n = 0;
    char *list = strdup(argv[1]);
    for (char *tok = strtok(list, ","); tok && n < 16; tok = strtok(NULL, ",")) pids[n++] = atoi(tok);
    double secs = atof(argv[2]);

    snap_t a[16], b[16];
    for (int i = 0; i < n; i++) take(pids[i], &a[i]);
    double t0 = now_s();
    usleep((useconds_t)(secs * 1e6));
    for (int i = 0; i < n; i++) take(pids[i], &b[i]);
    double wall = now_s() - t0;

    for (int i = 0; i < n; i++) {
        if (!a[i].ok_ru || !b[i].ok_ru) {
            printf("{\"pid\":%d,\"error\":\"rusage unavailable\"}\n", pids[i]);
            continue;
        }
        struct rusage_info_v6 *x = &a[i].ru, *y = &b[i].ru;
        double cpu = ticks_to_ms((y->ri_user_time - x->ri_user_time) + (y->ri_system_time - x->ri_system_time));
        double child = ticks_to_ms((y->ri_child_user_time - x->ri_child_user_time) +
                                   (y->ri_child_system_time - x->ri_child_system_time));
        uint64_t wk = (y->ri_pkg_idle_wkups - x->ri_pkg_idle_wkups) + (y->ri_interrupt_wkups - x->ri_interrupt_wkups);
        long long csw = 0, sysc = 0;
        int threads = 0;
        if (a[i].ok_ti && b[i].ok_ti) {
            csw = b[i].ti.pti_csw - a[i].ti.pti_csw;
            sysc = (b[i].ti.pti_syscalls_mach - a[i].ti.pti_syscalls_mach) +
                   (b[i].ti.pti_syscalls_unix - a[i].ti.pti_syscalls_unix);
            threads = b[i].ti.pti_threadnum;
        }
        printf("{\"pid\":%d,\"wall_s\":%.2f,\"cpu_ms\":%.2f,\"cpu_pct\":%.3f,\"child_cpu_ms\":%.2f,"
               "\"instructions\":%llu,\"cycles\":%llu,\"energy_mj\":%.3f,\"wakeups\":%llu,"
               "\"ctx_switches\":%lld,\"syscalls\":%lld,\"threads\":%d,\"footprint_mb\":%.1f,"
               "\"disk_write_kb\":%.1f}\n",
               pids[i], wall, cpu, cpu / (wall * 10.0), child,
               (unsigned long long)(y->ri_instructions - x->ri_instructions),
               (unsigned long long)(y->ri_cycles - x->ri_cycles),
               (y->ri_energy_nj - x->ri_energy_nj) / 1e6, (unsigned long long)wk, csw, sysc, threads,
               y->ri_phys_footprint / 1048576.0,
               (y->ri_diskio_byteswritten - x->ri_diskio_byteswritten) / 1024.0);
    }
    return 0;
}
