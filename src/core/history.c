#include "history.h"

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAGIC 0x4c45414eu  // "LEAN": format tag kept so existing history files stay readable
#define VERSION 2

typedef struct {
    uint32_t minute;  // unix minute stored in this slot (0 = empty)
    float avg[HS_COUNT];
    float max[HS_COUNT];
    uint8_t present;  // bit per series
    uint8_t pad[3];
    char top[24];     // process that used the most CPU in this minute ("" = none)
    float top_share;
} slot_t;

typedef struct {
    uint32_t magic, version, series, minutes;
    slot_t slot[HISTORY_MINUTES];
} file_t;

static file_t *g_file;
static int g_mapped;
static uint32_t g_cur_minute;
static double g_sum[HS_COUNT];
static float g_max[HS_COUNT];
static uint32_t g_cnt[HS_COUNT];

int history_open(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0 && ftruncate(fd, sizeof(file_t)) == 0) {
        void *p = mmap(NULL, sizeof(file_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            g_file = p;
            g_mapped = 1;
        }
    }
    if (fd >= 0) close(fd);
    if (!g_file) g_file = calloc(1, sizeof(file_t));
    if (g_file->magic != MAGIC || g_file->version != VERSION || g_file->series != HS_COUNT ||
        g_file->minutes != HISTORY_MINUTES) {
        memset(g_file, 0, sizeof(file_t));
        g_file->magic = MAGIC;
        g_file->version = VERSION;
        g_file->series = HS_COUNT;
        g_file->minutes = HISTORY_MINUTES;
    }
    return g_mapped ? 0 : -1;
}

static void flush_minute(void) {
    if (!g_cur_minute || !g_file) return;
    slot_t *s = &g_file->slot[g_cur_minute % HISTORY_MINUTES];
    slot_t n = {.minute = g_cur_minute};
    for (int i = 0; i < HS_COUNT; i++) {
        if (!g_cnt[i]) continue;
        n.avg[i] = (float)(g_sum[i] / g_cnt[i]);
        n.max[i] = g_max[i];
        n.present |= (uint8_t)(1u << i);
    }
    if (n.present) {
        if (s->minute == n.minute) { memcpy(n.top, s->top, sizeof n.top); n.top_share = s->top_share; }
        *s = n;
    }
    memset(g_sum, 0, sizeof g_sum);
    memset(g_cnt, 0, sizeof g_cnt);
    for (int i = 0; i < HS_COUNT; i++) g_max[i] = -INFINITY;
}

void history_add(double now, const float *v, unsigned mask) {
    uint32_t minute = (uint32_t)(now / 60.0);
    if (minute != g_cur_minute) {
        flush_minute();
        g_cur_minute = minute;
    }
    for (int i = 0; i < HS_COUNT; i++) {
        if (!(mask & (1u << i)) || !isfinite(v[i])) continue;
        g_sum[i] += v[i];
        if (!g_cnt[i] || v[i] > g_max[i]) g_max[i] = v[i];
        g_cnt[i]++;
    }
}

int history_series(int series, double now, float *avg, float *max) {
    uint32_t minute = (uint32_t)(now / 60.0);
    int n = 0;
    for (int i = 0; i < HISTORY_MINUTES; i++) {
        uint32_t m = minute - (HISTORY_MINUTES - 1) + (uint32_t)i;
        avg[i] = max[i] = NAN;
        if (m == g_cur_minute && g_cnt[series]) {  // the minute in progress
            avg[i] = (float)(g_sum[series] / g_cnt[series]);
            max[i] = g_max[series];
            n++;
            continue;
        }
        const slot_t *s = g_file ? &g_file->slot[m % HISTORY_MINUTES] : NULL;
        if (s && s->minute == m && (s->present & (1u << series))) {
            avg[i] = s->avg[series];
            max[i] = s->max[series];
            n++;
        }
    }
    return n;
}

void history_close(void) {
    flush_minute();
    if (g_mapped) msync(g_file, sizeof(file_t), MS_ASYNC);
}

void history_set_top(double t, const char *name, float share) {
    if (!g_file) return;
    uint32_t minute = (uint32_t)(t / 60.0);
    slot_t *s = &g_file->slot[minute % HISTORY_MINUTES];
    if (s->minute != minute) {  // minute not flushed yet: create the slot now; flush keeps top
        memset(s, 0, sizeof *s);
        s->minute = minute;
    }
    strlcpy(s->top, name, sizeof s->top);
    s->top_share = share;
}

int history_spikes(double now, int window_min, float min_avg, spike_t *out, int n) {
    uint32_t minute = (uint32_t)(now / 60.0);
    int count = 0;
    for (int back = 0; back < window_min && back < HISTORY_MINUTES && count < n; back++) {
        uint32_t m = minute - (uint32_t)back;
        const slot_t *s = g_file ? &g_file->slot[m % HISTORY_MINUTES] : NULL;
        if (!s || s->minute != m || !s->top[0] || !(s->present & (1u << HS_CPU)) || s->avg[HS_CPU] < min_avg) continue;
        spike_t *o = &out[count++];
        o->when = m * 60.0;
        o->cpu_avg = s->avg[HS_CPU];
        o->cpu_max = s->max[HS_CPU];
        o->top_share = s->top_share;
        memcpy(o->top, s->top, sizeof o->top);
    }
    return count;
}
