#include "procs.h"

#include "fastio.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

// Each user client of the GPU driver records its creator ("pid 123, Name") and the GPU
// time it has used (AppUsage: accumulatedGPUTime in ns, one entry per API). Readable
// for every user's processes. One property read costs ~38k instructions, so clients
// that recently used the GPU are read every refresh and the rest on a rotation;
// utilization the tracked clients don't explain sweeps everything next time.

#define MAXCL 512
#define ENUM_EVERY 30  // new clients are found by listing the accelerator's children
#define COLD_EVERY 30  // clients without new GPU time; new load also triggers a sweep
#define SWEEP_GAP 5    // minimum refreshes between triggered sweeps
#define LINGER 15      // refreshes a process stays listed after its last GPU use (keeps the panel layout still)
#define MIN_SHARE 0.0005f

typedef struct {
    io_object_t o;  // the send right; stable while held, so it identifies the client
    pid_t pid;
    uint64_t start;
    int64_t gpu;    // ns at the last read, -1 before the first
    uint64_t t;     // mach time of that read
    float frac;     // share of the GPU over the last interval
    uint8_t idle;   // consecutive reads without new GPU time
    uint8_t seen;
    uint32_t active_at;  // g_refresh + 1 at the last read with at least MIN_SHARE, 0 never
    char comm[17];  // short name, for processes proc_name can't see
} client_t;

static client_t g_cl[MAXCL];
static int g_n;
static io_service_t g_accel;
static uint32_t g_refresh;
static unsigned g_reads;
static int g_force;
static uint32_t g_swept_at;
static float g_unexpl_base;
static int g_base_set;
static double g_tick_ns;
static const fio_key_t kGpuTime = FIO_KEY("accumulatedGPUTime");

unsigned gpuprocs_last_reads(void) { return g_reads; }

// Clients and their last readings are kept: after reopening, one read of each finds
// which used the GPU in between, and only those are read again for exact shares.
void gpuprocs_reset(void) {
    for (int i = 0; i < g_n; i++) g_cl[i].active_at = 0;
    g_refresh = 0;
    g_force = 0;
    g_base_set = 0;
}

static pid_t creator_pid(io_object_t o) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(o, CFSTR("IOUserClientCreator"), kCFAllocatorDefault, 0);
    char s[64] = "";
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, s, sizeof s, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    return strncmp(s, "pid ", 4) ? 0 : (pid_t)atoi(s + 4);
}

static void enumerate(void) {
    for (int i = 0; i < g_n; i++) g_cl[i].seen = 0;
    io_iterator_t it;
    if (IORegistryEntryGetChildIterator(g_accel, kIOServicePlane, &it) != KERN_SUCCESS) return;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        int i = 0;
        while (i < g_n && g_cl[i].o != o) i++;
        if (i < g_n) {  // same port name: a client we already hold
            g_cl[i].seen = 1;
            IOObjectRelease(o);
            continue;
        }
        pid_t pid = creator_pid(o);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
        struct kinfo_proc kp;
        size_t len = sizeof kp;
        // sysctl, unlike proc_pidinfo, also works for other users' processes (WindowServer)
        if (pid <= 0 || g_n == MAXCL || sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof kp) {
            IOObjectRelease(o);
            continue;
        }
        struct timeval st = kp.kp_proc.p_starttime;
        client_t *c = &g_cl[g_n++];
        *c = (client_t){.o = o, .pid = pid, .start = st.tv_sec * 1000000ull + st.tv_usec, .gpu = -1, .seen = 1};
        memcpy(c->comm, kp.kp_proc.p_comm, sizeof c->comm - 1);
    }
    int complete = IOIteratorIsValid(it);  // false when the children changed mid-listing
    IOObjectRelease(it);
    for (int i = 0; i < g_n && complete; i++)
        if (!g_cl[i].seen) g_cl[i].pid = 0;  // removed below
}

static void read_client(client_t *c, uint64_t now) {
    int64_t gpu = 0;
    g_reads++;
    if (fio_sum_key(c->o, "AppUsage", kGpuTime, &gpu) < 0) {  // client closed
        c->pid = 0;
        return;
    }
    if (c->gpu >= 0 && gpu >= c->gpu && now > c->t) {
        c->frac = (float)((double)(gpu - c->gpu) / ((double)(now - c->t) * g_tick_ns));
        c->idle = gpu == c->gpu ? (uint8_t)(c->idle < 255 ? c->idle + 1 : 255) : 0;
        if (c->frac >= MIN_SHARE && g_refresh) c->active_at = g_refresh + 1;  // not use from while closed
    }
    c->gpu = gpu;
    c->t = now;
}

int gpuprocs_sample(float device_util, proc_row_t *rows, int *n, int max) {
    *n = 0;
    g_reads = 0;
    if (!g_accel) {
        g_accel = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_tick_ns = (double)tb.numer / tb.denom;
    }
    if (!g_accel) return 0;
    int sweep = g_refresh == 0 || (g_force && g_refresh - g_swept_at >= SWEEP_GAP);
    if (sweep) g_force = 0, g_swept_at = g_refresh;
    if (sweep || g_refresh % ENUM_EVERY == 0) enumerate();
    uint64_t now = mach_absolute_time();
    for (int i = 0; i < g_n; i++) {
        client_t *c = &g_cl[i];
        if (sweep || c->gpu < 0 || c->idle == 0 || (g_refresh + (uint32_t)i) % COLD_EVERY == 0) read_client(c, now);
    }
    g_refresh++;

    // Sum per process (a process can hold several clients) and rank.
    proc_row_t all[64];
    int na = 0;
    double explained = 0;
    for (int i = 0; i < g_n; i++) {
        client_t *c = &g_cl[i];
        if (c->pid <= 0) {
            IOObjectRelease(c->o);
            g_cl[i--] = g_cl[--g_n];
            continue;
        }
        explained += c->frac;
        if (!c->active_at || g_refresh - c->active_at >= LINGER) continue;
        int k = 0;
        while (k < na && !(all[k].pid == c->pid && all[k].start == c->start)) k++;
        if (k < na) all[k].cpu += c->frac;
        else if (na < 64) {
            all[na] = (proc_row_t){.pid = c->pid, .cpu = c->frac, .start = c->start};
            strlcpy(all[na++].name, c->comm, sizeof all[0].name);
        }
    }
    for (int k = 0; k < na; k++) {
        int pos = *n < max ? (*n)++ : max;
        while (pos > 0 && rows[pos - 1].cpu < all[k].cpu) {
            if (pos < max) rows[pos] = rows[pos - 1];
            pos--;
        }
        if (pos < max) rows[pos] = all[k];
    }
    for (int k = 0; k < *n; k++) {
        char full[64];  // proc_name() fails for buffers smaller than 2 * MAXCOMLEN
        if (proc_name(rows[k].pid, full, sizeof full) > 0) strlcpy(rows[k].name, full, sizeof rows[k].name);
    }

    // Utilization the tracked clients don't explain, relative to a slowly adapting
    // baseline (driver work belongs to no client): sweep everything next time.
    if (device_util >= 0 && g_refresh > 2) {
        float unexpl = device_util - (float)explained;
        if (!g_base_set) g_unexpl_base = unexpl, g_base_set = 1;  // can be negative: client time overlaps
        else if (unexpl - g_unexpl_base > 0.1f) { g_force = 1; g_unexpl_base = unexpl; }
        else g_unexpl_base += 0.2f * (unexpl - g_unexpl_base);
    }
    return g_refresh >= 2;
}
