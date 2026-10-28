#include "procs.h"

#include <libproc.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <stdio.h>
#include <unistd.h>

// Tiered scanning: per-process CPU reads are expensive, so each refresh lists pids,
// reads new and hot processes (recent CPU, memory rows on screen), and re-reads
// warm and idle ones on staggered rotations. Each row's CPU% is exact over its own interval.

#define TABLE 4096  // open addressing, power of two
#define HOT_MAX 48

typedef struct {
    pid_t pid;      // 0 empty, -1 deleted
    uint32_t seen;  // scan generation of the last successful read
    uint64_t cpu;   // user+system, mach ticks
    uint64_t t;     // mach_absolute_time of that read
    float frac;     // CPU fraction over the last interval
    uint64_t mem;
    uint8_t idle;   // consecutive reads with no CPU time
    uint64_t start; // process start (mach abs): distinguishes a recycled pid
} slot_t;

// Names of rows on screen, keyed by (pid, start); avoids proc_name per row per refresh.
typedef struct { pid_t pid; uint64_t start; char name[64]; } name_t;
static name_t g_names[32];
static uint32_t g_name_next;

static void fill_name(proc_row_t *r, uint64_t start) {
    for (int i = 0; i < 32; i++)
        if (g_names[i].pid == r->pid && g_names[i].start == start) { memcpy(r->name, g_names[i].name, sizeof r->name); return; }
    name_t *n = &g_names[g_name_next++ % 32];
    n->pid = r->pid;
    n->start = start;
    if (proc_name(r->pid, n->name, sizeof n->name) <= 0) n->name[0] = 0;
    memcpy(r->name, n->name, sizeof r->name);
}

static slot_t g_tab[TABLE];
static uint32_t g_scan;
static uint32_t g_refresh;
static int g_full_every = 0;  // 0: tiered mode
static int g_cold_period = 12;  // the CPU trigger below catches wake-ups
static float g_busy_cores = -1;  // machine-wide busy cores (from host CPU stats), -1 = unknown
static float g_unexpl_base = -1; // baseline of CPU not explained by the processes we track
static int g_force_sweep;
static int g_warm_period = 6;
static float g_hot_frac = 0.005f;  // 0.5% of a core
static pid_t g_hot[HOT_MAX];
static int g_nhot;
static pid_t g_pids[8192];
static double g_tick_ns;

void procs_set_full_every(int n) { g_full_every = n < 0 ? 0 : n; }
void procs_set_cold_period(int n) { g_cold_period = n < 1 ? 1 : n; }
void procs_set_busy_cores(float busy) { g_busy_cores = busy; }
void procs_set_warm(int period, float hot_frac) { g_warm_period = period < 1 ? 1 : period; g_hot_frac = hot_frac; }

void procs_reset(void) {
    memset(g_tab, 0, sizeof(g_tab));
    g_unexpl_base = -1;
    g_force_sweep = 0;
    memset(g_names, 0, sizeof(g_names));
    g_scan = g_refresh = 0;
    g_nhot = 0;
}

static slot_t *lookup(pid_t pid, int create) {
    uint32_t h = (uint32_t)pid * 2654435761u;
    slot_t *reuse = NULL;
    for (uint32_t i = 0; i < TABLE; i++) {
        slot_t *s = &g_tab[(h + i) & (TABLE - 1)];
        if (s->pid == pid) return s;
        if (s->pid == 0) {
            if (!create) return NULL;
            return reuse ? reuse : s;
        }
        // deleted, or not seen in the last two full scans (process gone)
        if (!reuse && (s->pid == -1 || s->seen + 2 <= g_scan)) reuse = s;
    }
    return create ? reuse : NULL;
}

static unsigned g_reads;  // per-process reads in the last refresh (benchmarks)
unsigned procs_last_reads(void) { return g_reads; }

static int read_pid(pid_t pid, uint64_t now) {
    struct rusage_info_v2 ru;
    g_reads++;
    slot_t *s = lookup(pid, 1);
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ru) != 0) {
        if (s && s->pid == pid) s->pid = -1;
        return 0;
    }
    if (!s) return 0;
    uint64_t cpu = ru.ri_user_time + ru.ri_system_time;
    if (s->pid == pid && s->start != ru.ri_proc_start_abstime) s->pid = -1;  // pid was recycled
    s->start = ru.ri_proc_start_abstime;
    if (s->pid == pid && s->t && now > s->t && cpu >= s->cpu) {
        s->frac = (float)((double)(cpu - s->cpu) / (double)(now - s->t));  // both in mach ticks
        s->idle = cpu == s->cpu ? (uint8_t)(s->idle < 255 ? s->idle + 1 : 255) : 0;
    } else if (s->pid != pid) {
        s->frac = 0;
        s->idle = 0;
    }
    s->pid = pid;
    s->cpu = cpu;
    s->t = now;
    s->mem = ru.ri_phys_footprint;
    s->seen = g_scan;
    return 1;
}

static void insert_top(proc_row_t *rows, int *count, int n, proc_row_t r, int by_cpu) {
    int c = *count, pos = c;
    while (pos > 0 && (by_cpu ? rows[pos - 1].cpu < r.cpu : rows[pos - 1].memory < r.memory)) pos--;
    if (pos >= n) return;
    int last = c < n ? c : n - 1;
    memmove(&rows[pos + 1], &rows[pos], (size_t)(last - pos) * sizeof(proc_row_t));
    rows[pos] = r;
    if (c < n) (*count)++;
}

int procs_sample(double dt, proc_row_t *by_cpu, int *ncpu, proc_row_t *by_mem, int *nmem, int n) {
    (void)dt;  // each process is measured over its own interval
    if (!g_tick_ns) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_tick_ns = (double)tb.numer / tb.denom;
    }
    uint64_t now = mach_absolute_time();
    g_reads = 0;
    if (g_full_every > 0) {  // reference mode: full scan every n refreshes, hot set in between
        int full = g_scan < 2 || (g_refresh % (uint32_t)g_full_every) == 0;
        g_refresh++;
        if (full) {
            g_scan++;
            int bytes = proc_listpids(PROC_UID_ONLY, getuid(), g_pids, sizeof(g_pids));
            int np = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;
            for (int i = 0; i < np; i++)
                if (g_pids[i] > 0) read_pid(g_pids[i], now);
        } else {
            for (int i = 0; i < g_nhot; i++) read_pid(g_hot[i], now);
        }
    } else {
        // Tiered: every listed pid is marked alive; only some are read.
        g_scan++;
        g_refresh++;
        int bytes = proc_listpids(PROC_UID_ONLY, getuid(), g_pids, sizeof(g_pids));
        int np = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;
        int warmup = g_scan <= 2;  // two complete reads so every process has a CPU fraction
        int sweep = g_force_sweep;  // CPU appeared that the tracked processes don't explain
        g_force_sweep = 0;
        for (int i = 0; i < np; i++) {
            pid_t pid = g_pids[i];
            if (pid <= 0) continue;
            slot_t *sl = lookup(pid, 1);
            // hot (>= g_hot_frac): every refresh; warm: every g_warm_period; idle 3+ reads: every g_cold_period.
            uint32_t phase = (uint32_t)pid + g_refresh;
            int read = warmup || sweep || !sl || sl->pid != pid || sl->frac >= g_hot_frac ||
                       (sl->idle < 3 ? phase % (uint32_t)g_warm_period == 0 : phase % (uint32_t)g_cold_period == 0);
            if (read) read_pid(pid, now);
            else sl->seen = g_scan;  // alive, value unchanged (idle)
        }
        for (int i = 0; i < g_nhot; i++) {  // memory rows on screen stay fresh
            slot_t *sl = lookup(g_hot[i], 0);
            if (sl && sl->seen == g_scan && sl->t != now) read_pid(g_hot[i], now);
        }
    }

    // Rank everything alive (seen in the latest full scan, or refreshed since).
    *ncpu = *nmem = 0;
    proc_row_t hot_rows[HOT_MAX];
    int nhot_rows = 0;
    double explained = 0;
    for (int i = 0; i < TABLE; i++) {
        slot_t *s = &g_tab[i];
        if (s->pid <= 0 || s->seen != g_scan) continue;
        explained += s->frac;
        proc_row_t r = {.pid = s->pid, .cpu = s->frac, .memory = s->mem, .start = s->start};
        insert_top(by_cpu, ncpu, n, r, 1);
        insert_top(by_mem, nmem, n, r, 0);
        if (g_full_every > 0 && s->frac > 0.001f) insert_top(hot_rows, &nhot_rows, HOT_MAX - n, r, 1);
    }
    // Wake-up trigger: when machine-wide busy CPU rises more than half a core above
    // what the tracked processes explain (relative to a slowly adapting baseline, as
    // other users' processes are invisible to us), sweep the idle processes next time.
    if (g_full_every == 0 && g_busy_cores >= 0 && g_scan > 2) {
        float unexpl = g_busy_cores - (float)explained;
        if (g_unexpl_base < 0) g_unexpl_base = unexpl;
        if (unexpl - g_unexpl_base > 0.5f) { g_force_sweep = 1; g_unexpl_base = unexpl; }
        else g_unexpl_base += 0.2f * (unexpl - g_unexpl_base);
    }
    // Next hot set: recent CPU users plus the memory leaders on screen.
    g_nhot = 0;
    if (g_full_every > 0)
        for (int i = 0; i < nhot_rows; i++) g_hot[g_nhot++] = hot_rows[i].pid;
    for (int i = 0; i < *nmem && g_nhot < HOT_MAX; i++) g_hot[g_nhot++] = by_mem[i].pid;

    for (int i = 0; i < *ncpu; i++) fill_name(&by_cpu[i], by_cpu[i].start);
    for (int i = 0; i < *nmem; i++) fill_name(&by_mem[i], by_mem[i].start);
    return g_scan >= 2;
}

// ---- spike attribution ------------------------------------------------------------
typedef struct { pid_t pid; uint64_t start, cpu; } aslot_t;
#define ATABLE 2048  // ~650 own processes; 48 KB per table
static aslot_t g_atab[ATABLE];
static uint64_t g_alast;

int procs_top_since_last(char *name, size_t cap, float *share) {
    static aslot_t next[ATABLE];
    memset(next, 0, sizeof next);
    uint64_t now = mach_absolute_time();
    int bytes = proc_listpids(PROC_UID_ONLY, getuid(), g_pids, sizeof(g_pids));
    int np = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;
    uint64_t best = 0;
    pid_t best_pid = 0;
    for (int i = 0; i < np; i++) {
        pid_t pid = g_pids[i];
        if (pid <= 0) continue;
        struct rusage_info_v2 ru;
        if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ru) != 0) continue;
        uint64_t cpu = ru.ri_user_time + ru.ri_system_time;
        uint32_t h = (uint32_t)pid * 2654435761u;
        uint64_t prev = UINT64_MAX;
        for (uint32_t k = 0; k < ATABLE; k++) {
            aslot_t *a = &g_atab[(h + k) & (ATABLE - 1)];
            if (!a->pid) break;
            if (a->pid == pid) { if (a->start == ru.ri_proc_start_abstime) prev = a->cpu; break; }
        }
        // a process that started since the last scan counts from zero
        uint64_t used = prev == UINT64_MAX ? (g_alast && ru.ri_proc_start_abstime > g_alast ? cpu : 0) : (cpu >= prev ? cpu - prev : 0);
        if (used > best) { best = used; best_pid = pid; }
        for (uint32_t k = 0; k < ATABLE; k++) {
            aslot_t *a = &next[(h + k) & (ATABLE - 1)];
            if (!a->pid) { a->pid = pid; a->start = ru.ri_proc_start_abstime; a->cpu = cpu; break; }
        }
    }
    memcpy(g_atab, next, sizeof g_atab);
    uint64_t prev_t = g_alast;
    g_alast = now;
    if (!prev_t || !best_pid || now <= prev_t) return 0;
    *share = (float)((double)best / (double)(now - prev_t));
    char full[64];  // proc_name() fails for buffers smaller than 2 * MAXCOMLEN
    if (proc_name(best_pid, full, sizeof full) > 0) strlcpy(name, full, cap);
    else snprintf(name, cap, "pid %d", best_pid);
    return 1;
}
