#include "smc.h"

#include <IOKit/IOKitLib.h>
#include <string.h>
#include <sys/sysctl.h>

typedef struct { uint32_t dataSize, dataType; uint8_t dataAttributes; } smc_keyinfo_t;
typedef struct {
    uint32_t key;
    uint8_t vers[6];
    uint8_t plimit[16];
    smc_keyinfo_t keyInfo;
    uint8_t result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} smc_data_t;
_Static_assert(sizeof(smc_data_t) == 80, "AppleSMC struct is 80 bytes");

#define FCC(s) ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])
#define T_FLT 0x666c7420u  /* "flt " */
#define T_SP78 0x73703738u /* "sp78" */

typedef struct {
    uint32_t key;
    smc_keyinfo_t info;
    float value;
    uint8_t group;  // 0 cpu, 1 gpu
} sensor_t;

static io_connect_t g_conn;
static int g_state;  // 0 untried, 1 open, -1 unavailable
static sensor_t g_temps[48];
static int g_ntemps, g_rr;
static sensor_t g_fans[4], g_fanmax[4], g_power;
static int g_nfans, g_have_power;

static int call(smc_data_t *in, smc_data_t *out) {
    size_t osz = sizeof(*out);
    return IOConnectCallStructMethod(g_conn, 2, in, sizeof(*in), out, &osz) == KERN_SUCCESS && out->result == 0;
}

static int keyinfo(uint32_t key, smc_keyinfo_t *ki) {
    smc_data_t in = {0}, out = {0};
    in.key = key;
    in.data8 = 9;
    if (!call(&in, &out)) return 0;
    *ki = out.keyInfo;
    return ki->dataSize > 0 && ki->dataSize <= 32;
}

static int readf(sensor_t *s) {
    smc_data_t in = {0}, out = {0};
    in.key = s->key;
    in.keyInfo.dataSize = s->info.dataSize;
    in.data8 = 5;
    if (!call(&in, &out)) return 0;
    if (s->info.dataType == T_FLT) memcpy(&s->value, out.bytes, 4);
    else if (s->info.dataType == T_SP78) s->value = (float)(int16_t)(out.bytes[0] << 8 | out.bytes[1]) / 256.0f;
    else return 0;
    return 1;
}

static int add(sensor_t *dst, const char *k, uint8_t group) {
    dst->key = FCC(k);
    dst->group = group;
    return keyinfo(dst->key, &dst->info) && (dst->info.dataType == T_FLT || dst->info.dataType == T_SP78);
}

// CPU/GPU temperature keys averaged by the original Stats (Modules/Sensors/values.swift).
static const char *const kCPU[][19] = {
    /* m1 */ {"Tp01", "Tp05", "Tp09", "Tp0b", "Tp0D", "Tp0H", "Tp0L", "Tp0P", "Tp0T", "Tp0X", 0},
    /* m2 */ {"Tp01", "Tp05", "Tp09", "Tp0b", "Tp0D", "Tp0f", "Tp0j", "Tp0X", "Tp1h", "Tp1l", "Tp1p", "Tp1t", 0},
    /* m3 */ {"Te05", "Te0L", "Te0P", "Te0S", "Tf04", "Tf09", "Tf0A", "Tf0B", "Tf0D", "Tf0E", "Tf44", "Tf49", "Tf4A", "Tf4B", "Tf4D", "Tf4E", 0},
    /* m4 */ {"Te05", "Te09", "Te0H", "Te0S", "Tp01", "Tp05", "Tp09", "Tp0b", "Tp0D", "Tp0e", "Tp0V", "Tp0Y", 0},
    /* m5 */ {"Tp00", "Tp04", "Tp08", "Tp0a", "Tp0C", "Tp0d", "Tp0g", "Tp0G", "Tp0j", "Tp0K", "Tp0m", "Tp0O", "Tp0p", "Tp0R", "Tp0u", "Tp0U", "Tp0X", "Tp0y", 0},
    /* m6 */ {"Te07", "Te08", "Te09", "Te0c", "Te0h", "Te0i", "Tp05", "Tp07", "Tp09", "Tp0b", "Tp0d", "Tp0E", "Tp0g", "Tp0G", "Tp0I", "Tp0j", "Tp0L", "Tp0m", 0},
};
static const char *const kGPU[][13] = {
    /* m1 */ {"Tg05", "Tg0D", "Tg0L", "Tg0T", 0},
    /* m2 */ {"Tg0f", "Tg0j", 0},
    /* m3 */ {"Tf14", "Tf18", "Tf19", "Tf1A", "Tf24", "Tf28", "Tf29", "Tf2A", 0},
    /* m4 */ {"Tg0d", "Tg0e", "Tg0j", "Tg0k", "Tg0K", "Tg0L", 0},
    /* m5 */ {"Tg0d", "Tg0g", "Tg0j", "Tg0U", "Tg0X", "Tg1c", "Tg1g", "Tg1Y", 0},
    /* m6 */ {"Tg07", "Tg09", "Tg0a", "Tg0b", "Tg0c", "Tg0d", "Tg1c", "Tg1d", "Tg1e", "Tg1f", "Tg1g", "Tg1h", 0},
};

static int chip_generation(void) {
    char brand[128] = {0};
    size_t sz = sizeof(brand);
    sysctlbyname("machdep.cpu.brand_string", brand, &sz, NULL, 0);
    const char *m = strstr(brand, "Apple M");
    if (!m || m[7] < '1' || m[7] > '6') return -1;
    return m[7] - '1';
}

int smc_open_once(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) return 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) return 0;
    g_state = 1;

    int gen = chip_generation();
    if (gen >= 0) {
        for (int i = 0; kCPU[gen][i] && g_ntemps < 48; i++)
            if (add(&g_temps[g_ntemps], kCPU[gen][i], 0)) g_ntemps++;
        for (int i = 0; kGPU[gen][i] && g_ntemps < 48; i++)
            if (add(&g_temps[g_ntemps], kGPU[gen][i], 1)) g_ntemps++;
    } else {  // Intel: package / GPU die
        if (add(&g_temps[g_ntemps], "TC0P", 0)) g_ntemps++;
        if (add(&g_temps[g_ntemps], "TG0P", 1)) g_ntemps++;
    }
    for (int i = 0; i < 4; i++) {
        char k[5] = {'F', (char)('0' + i), 'A', 'c', 0}, mx[5] = {'F', (char)('0' + i), 'M', 'x', 0};
        if (!add(&g_fans[g_nfans], k, 0)) break;
        if (add(&g_fanmax[g_nfans], mx, 0)) readf(&g_fanmax[g_nfans]);
        g_nfans++;
    }
    g_have_power = add(&g_power, "PSTR", 0);
    for (int i = 0; i < g_ntemps; i++) readf(&g_temps[i]);  // full read once
    return 1;
}

void smc_sensors(snapshot_t *s) {
    if (!smc_open_once()) return;
    // Refresh 4 temperature keys per call, round-robin.
    for (int n = 0; n < 4 && g_ntemps; n++) {
        readf(&g_temps[g_rr]);
        g_rr = (g_rr + 1) % g_ntemps;
    }
    double sum[2] = {0}, cnt[2] = {0};
    for (int i = 0; i < g_ntemps; i++) {
        float v = g_temps[i].value;
        if (v > 5 && v < 120) { sum[g_temps[i].group] += v; cnt[g_temps[i].group] += 1; }
    }
    s->temp_cpu = cnt[0] ? (float)(sum[0] / cnt[0]) : 0;
    s->temp_gpu = cnt[1] ? (float)(sum[1] / cnt[1]) : 0;
    s->nfans = g_nfans;
    for (int i = 0; i < g_nfans; i++) {
        readf(&g_fans[i]);
        s->fan_rpm[i] = g_fans[i].value < 0 ? 0 : g_fans[i].value;
        s->fan_max[i] = g_fanmax[i].value;
    }
    if (g_have_power && readf(&g_power)) s->power_system = g_power.value;
}
