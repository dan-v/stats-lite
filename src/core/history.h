// history: 24 hours of per-minute averages and maxima, persisted in an mmap'd file.
// Ticks add to in-RAM accumulators; each finished minute is one plain store into
// the mapping, and the kernel writes the dirty page back on its own.
#pragma once
#include <stdint.h>

enum { HS_CPU, HS_GPU, HS_MEM, HS_DISK, HS_NET_IN, HS_NET_OUT, HS_TEMP, HS_COUNT };
#define HISTORY_MINUTES 1440

int history_open(const char *path);  // 0 on success; history stays in RAM on failure
void history_add(double now_unix, const float *values, unsigned mask);  // mask: bit per series present
// Copies the last `HISTORY_MINUTES` minutes for one series, oldest first.
// Missing minutes are NaN. Returns the number of minutes with data.
int history_series(int series, double now_unix, float *avg, float *max);
void history_close(void);

// Spike attribution: name of the process that used the most CPU in `minute_unix`
// (a unix time inside that minute) and its share of one core.
void history_set_top(double minute_unix, const char *name, float share);

typedef struct {
    double when;      // unix time of the minute
    float cpu_avg;    // HS_CPU average (0..100)
    float cpu_max;
    float top_share;  // fraction of one core
    char top[24];
} spike_t;
// The n busiest attributed minutes of the last `window_min` minutes, newest first.
int history_spikes(double now_unix, int window_min, float min_avg, spike_t *out, int n);
