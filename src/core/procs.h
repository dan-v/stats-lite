// procs: top processes by CPU and memory via libproc, while the panel is visible.
// Current user's processes only: the kernel denies rusage for others without a
// private entitlement.
#pragma once
#include <sys/types.h>
#include <stdint.h>

typedef struct {
    pid_t pid;
    float cpu;         // fraction of one core
    uint64_t memory;   // phys_footprint bytes
    uint64_t start;    // process start time (identity together with pid)
    char name[64];
} proc_row_t;

// Fills up to n rows sorted by CPU and up to n rows sorted by memory. dt is the
// elapsed time since the previous call. Returns 1 when CPU deltas are valid.
int procs_sample(double dt, proc_row_t *by_cpu, int *ncpu, proc_row_t *by_mem, int *nmem, int n);
void procs_reset(void);
// Default is tiered scanning. n > 0 selects the reference mode: full scan every n
// refreshes (1 = every refresh), hot set only in between.
void procs_set_full_every(int n);
void procs_set_cold_period(int n);  // tiered: idle processes re-read every n refreshes
// tiered: processes below hot_frac of a core (but not idle) re-read every `period` refreshes
void procs_set_warm(int period, float hot_frac);
unsigned procs_last_reads(void);
// Machine-wide busy cores (cpu_total x logical CPUs), set before each refresh so the
// tiered scan can notice CPU no tracked process explains and sweep idle processes.
void procs_set_busy_cores(float busy);

// Spike attribution (independent of the tables above, works with the panel closed):
// the process that used the most CPU since the previous call, with its average
// fraction of one core in *share. Returns 0 on the first call (baseline only).
int procs_top_since_last(char *name, size_t cap, float *share);

// Top processes by GPU share, any user, from the GPU driver's per-client usage.
// device_util (0..1, < 0 unknown) lets the tiered reads notice untracked load.
// Rows use proc_row_t with .cpu holding the GPU share. Returns 1 when shares are valid.
int gpuprocs_sample(float device_util, proc_row_t *rows, int *n, int max);
void gpuprocs_reset(void);
unsigned gpuprocs_last_reads(void);
