// sampler: allocation-free metric collection. Every function here is called from
// the main thread once per tick, only for metrics something on screen needs.
#pragma once
#include <stdint.h>

enum {
    NEED_CPU = 1 << 0,         // total / user / system
    NEED_CPU_CORES = 1 << 1,   // per-core + cluster averages (panel)
    NEED_LOAD = 1 << 2,        // load averages (panel)
    NEED_MEM = 1 << 3,         // used / pressure
    NEED_MEM_DETAIL = 1 << 4,  // swap (panel)
    NEED_GPU = 1 << 5,         // device utilization
    NEED_DISK_SPACE = 1 << 6,  // free / total of the root volume
    NEED_DISK_IO = 1 << 7,     // read / write rates (panel)
    NEED_NET = 1 << 8,         // primary interface throughput
    NEED_SENSORS = 1 << 9,     // SMC temperatures / fans / power
    NEED_BATTERY = 1 << 10,    // battery level / state
};

#define MAX_CORES 64

typedef struct {
    // CPU (fractions 0..1)
    float cpu_total, cpu_user, cpu_system;
    int ncores, nclusters;
    float core[MAX_CORES];
    uint8_t core_cluster[MAX_CORES];  // index into cluster_*
    char cluster_name[3][16];
    float cluster_avg[3];
    double load[3];

    // Memory (bytes)
    uint64_t mem_total, mem_used, mem_app, mem_wired, mem_compressed, mem_cache, mem_free;
    int mem_pressure;  // 1 normal, 2 warning, 4 critical
    uint64_t swap_total, swap_used;

    // GPU (fractions 0..1)
    int gpu_ok;
    float gpu_util, gpu_render, gpu_tiler;

    // Disk (bytes, bytes/s)
    uint64_t disk_total, disk_free, disk_purgeable;
    double disk_read_bps, disk_write_bps;

    // Network
    int net_ok;
    char net_if[16];
    double net_in_bps, net_out_bps;
    uint64_t net_in_total, net_out_total;

    // Sensors
    float temp_cpu, temp_gpu, power_system;
    int nfans;
    float fan_rpm[4], fan_max[4];

    // Battery
    int batt_present, batt_charging, batt_ac, batt_minutes;  // minutes: -1 unknown
    float batt_level;

    // Bluetooth / accessory batteries
    int nbt;
    struct { char name[40]; float level; int charging; } bt[8];
} snapshot_t;

void sampler_init(void);
// Collects the metrics in `need` into *s. `dt` is seconds since the previous call
// for rate metrics (network, disk I/O).
void sampler_collect(unsigned need, double dt, snapshot_t *s);
// Starts an async purgeable-space refresh if the cached value is older than max_age.
void sampler_refresh_purgeable(double max_age_s);
// Called when the primary interface may have changed.
void sampler_net_invalidate(void);
// After a gap in sampling (sleep, lock, occlusion): rate metrics re-baseline.
void sampler_reprime_rates(void);
// Battery is event driven: the app calls this from the IOPS notification.
void sampler_battery(snapshot_t *s);
// Accessory batteries: IOPS accessory power sources plus BatteryPercent of Apple HID
// devices. Avoids CoreBluetooth, which needs a permission prompt.
void sampler_bluetooth(snapshot_t *s);
