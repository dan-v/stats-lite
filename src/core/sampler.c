#include "sampler.h"

#include "fastio.h"
#include "smc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach_time.h>

static host_t g_host;

// ---- CPU -------------------------------------------------------------------
static host_cpu_load_info_data_t g_cpu_prev;
static int g_cpu_primed;
static int g_ncpu, g_nclusters;
static uint8_t g_core_cluster[MAX_CORES];
static char g_cluster_name[3][16];
static uint32_t g_core_prev[MAX_CORES][CPU_STATE_MAX];
static int g_cores_primed;

static void cpu_topology_init(void) {
    int n = 0;
    size_t sz = sizeof(n);
    sysctlbyname("hw.logicalcpu", &n, &sz, NULL, 0);
    g_ncpu = n > MAX_CORES ? MAX_CORES : n;

    // Cluster type per logical CPU from the device tree ("E", "M", "P").
    char type[MAX_CORES] = {0};
    io_registry_entry_t cpus = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/cpus");
    io_iterator_t it = 0;
    if (cpus && IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it) == KERN_SUCCESS) {
        io_registry_entry_t c;
        while ((c = IOIteratorNext(it))) {
            CFTypeRef idv = IORegistryEntryCreateCFProperty(c, CFSTR("logical-cpu-id"), NULL, 0);
            CFTypeRef ct = IORegistryEntryCreateCFProperty(c, CFSTR("cluster-type"), NULL, 0);
            int id = -1;
            if (idv && CFGetTypeID(idv) == CFNumberGetTypeID()) CFNumberGetValue(idv, kCFNumberIntType, &id);
            else if (idv && CFGetTypeID(idv) == CFDataGetTypeID() && CFDataGetLength(idv) >= 4) memcpy(&id, CFDataGetBytePtr(idv), 4);
            if (id >= 0 && id < MAX_CORES && ct && CFGetTypeID(ct) == CFDataGetTypeID() && CFDataGetLength(ct) > 0)
                type[id] = (char)CFDataGetBytePtr(ct)[0];
            if (idv) CFRelease(idv);
            if (ct) CFRelease(ct);
            IOObjectRelease(c);
        }
        IOObjectRelease(it);
    }
    if (cpus) IOObjectRelease(cpus);

    // Order clusters from efficient to fastest: E, M, P. Name them by what exists.
    int has_e = 0, has_m = 0, has_p = 0;
    for (int i = 0; i < g_ncpu; i++) { has_e |= type[i] == 'E'; has_m |= type[i] == 'M'; has_p |= type[i] == 'P'; }
    int idx_e = -1, idx_m = -1, idx_p = -1;
    g_nclusters = 0;
    if (has_e) { idx_e = g_nclusters; strcpy(g_cluster_name[g_nclusters++], "Efficiency"); }
    if (has_m) { idx_m = g_nclusters; strcpy(g_cluster_name[g_nclusters++], "Performance"); }
    if (has_p) { idx_p = g_nclusters; strcpy(g_cluster_name[g_nclusters++], has_m ? "Super" : "Performance"); }
    for (int i = 0; i < g_ncpu; i++)
        g_core_cluster[i] = (uint8_t)(type[i] == 'E' ? idx_e : type[i] == 'M' ? idx_m : type[i] == 'P' ? idx_p : 0);
    if (!g_nclusters) { g_nclusters = 1; strcpy(g_cluster_name[0], "CPU"); }
}

static void cpu_total(snapshot_t *s) {
    host_cpu_load_info_data_t l;
    mach_msg_type_number_t c = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(g_host, HOST_CPU_LOAD_INFO, (host_info_t)&l, &c) != KERN_SUCCESS) return;
    if (g_cpu_primed) {
        uint32_t u = l.cpu_ticks[CPU_STATE_USER] - g_cpu_prev.cpu_ticks[CPU_STATE_USER];
        uint32_t sy = l.cpu_ticks[CPU_STATE_SYSTEM] - g_cpu_prev.cpu_ticks[CPU_STATE_SYSTEM];
        uint32_t id = l.cpu_ticks[CPU_STATE_IDLE] - g_cpu_prev.cpu_ticks[CPU_STATE_IDLE];
        uint32_t ni = l.cpu_ticks[CPU_STATE_NICE] - g_cpu_prev.cpu_ticks[CPU_STATE_NICE];
        double tot = (double)u + sy + id + ni;
        if (tot > 0) {
            s->cpu_user = (float)((u + ni) / tot);
            s->cpu_system = (float)(sy / tot);
            s->cpu_total = s->cpu_user + s->cpu_system;
        }
    }
    g_cpu_prev = l;
    g_cpu_primed = 1;
}

static void cpu_cores(snapshot_t *s) {
    natural_t n = 0;
    processor_info_array_t a = NULL;
    mach_msg_type_number_t cnt = 0;
    if (host_processor_info(g_host, PROCESSOR_CPU_LOAD_INFO, &n, &a, &cnt) != KERN_SUCCESS) return;
    int nc = (int)n > g_ncpu ? g_ncpu : (int)n;
    double sum[3] = {0}, num[3] = {0};
    for (int i = 0; i < nc; i++) {
        uint32_t *t = (uint32_t *)&a[i * CPU_STATE_MAX];
        if (g_cores_primed) {
            uint32_t u = t[CPU_STATE_USER] - g_core_prev[i][CPU_STATE_USER];
            uint32_t sy = t[CPU_STATE_SYSTEM] - g_core_prev[i][CPU_STATE_SYSTEM];
            uint32_t id = t[CPU_STATE_IDLE] - g_core_prev[i][CPU_STATE_IDLE];
            uint32_t ni = t[CPU_STATE_NICE] - g_core_prev[i][CPU_STATE_NICE];
            double tot = (double)u + sy + id + ni;
            float v = tot > 0 ? (float)((u + sy + ni) / tot) : 0;
            s->core[i] = v;
            sum[g_core_cluster[i]] += v;
            num[g_core_cluster[i]] += 1;
        }
        memcpy(g_core_prev[i], t, sizeof(g_core_prev[i]));
    }
    vm_deallocate(mach_task_self(), (vm_address_t)a, cnt * sizeof(integer_t));
    s->ncores = nc;
    s->nclusters = g_nclusters;
    for (int k = 0; k < g_nclusters; k++) {
        memcpy(s->cluster_name[k], g_cluster_name[k], 16);
        s->cluster_avg[k] = num[k] > 0 ? (float)(sum[k] / num[k]) : 0;
    }
    memcpy(s->core_cluster, g_core_cluster, sizeof(g_core_cluster));
    g_cores_primed = 1;
}

// ---- Memory ------------------------------------------------------------------
static uint64_t g_memsize;
static vm_size_t g_page;
static int g_mib_pressure[4], g_mib_swap[4];
static u_int g_mib_pressure_len, g_mib_swap_len;

static void mem(snapshot_t *s, int detail) {
    vm_statistics64_data_t v;
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    if (host_statistics64(g_host, HOST_VM_INFO64, (host_info64_t)&v, &c) != KERN_SUCCESS) return;
    uint64_t pg = g_page;
    uint64_t active = v.active_count * pg, inactive = v.inactive_count * pg, spec = v.speculative_count * pg;
    uint64_t wired = v.wire_count * pg, compressed = v.compressor_page_count * pg;
    uint64_t purgeable = v.purgeable_count * pg, external = v.external_page_count * pg;
    // Same definition the original Stats uses (Modules/RAM/readers.swift).
    uint64_t gross = active + inactive + spec + wired + compressed, reclaim = purgeable + external;
    uint64_t used = gross > reclaim ? gross - reclaim : 0;
    s->mem_total = g_memsize;
    s->mem_used = used;
    s->mem_free = g_memsize > used ? g_memsize - used : 0;
    s->mem_wired = wired;
    s->mem_compressed = compressed;
    s->mem_app = used > wired + compressed ? used - wired - compressed : 0;
    s->mem_cache = purgeable + external;
    int lvl = 1;
    size_t sz = sizeof(lvl);
    if (g_mib_pressure_len && sysctl(g_mib_pressure, g_mib_pressure_len, &lvl, &sz, NULL, 0) == 0) s->mem_pressure = lvl;
    if (detail && g_mib_swap_len) {
        struct xsw_usage x;
        sz = sizeof(x);
        if (sysctl(g_mib_swap, g_mib_swap_len, &x, &sz, NULL, 0) == 0) { s->swap_total = x.xsu_total; s->swap_used = x.xsu_used; }
    }
}

// ---- GPU -------------------------------------------------------------------
static io_service_t g_gpu;
static const fio_key_t g_gpu_keys[3] = {FIO_KEY("Device Utilization %"), FIO_KEY("Renderer Utilization %"),
                                        FIO_KEY("Tiler Utilization %")};

static void gpu(snapshot_t *s) {
    int64_t v[3] = {-1, -1, -1};
    if (!g_gpu || fio_read_numbers(g_gpu, "PerformanceStatistics", g_gpu_keys, 3, v) <= 0 || v[0] < 0) {
        s->gpu_ok = 0;
        return;
    }
    s->gpu_ok = 1;
    s->gpu_util = (float)(v[0] > 100 ? 100 : v[0]) / 100.0f;
    s->gpu_render = v[1] >= 0 ? (float)(v[1] > 100 ? 100 : v[1]) / 100.0f : 0;
    s->gpu_tiler = v[2] >= 0 ? (float)(v[2] > 100 ? 100 : v[2]) / 100.0f : 0;
}

// ---- Disk --------------------------------------------------------------------
static int g_root_fd = -1;
static io_registry_entry_t g_disk_driver;
static int64_t g_disk_prev_r = -1, g_disk_prev_w = -1;
static uint64_t g_disk_prev_t;  // mach time of the previous disk counter read
static _Atomic uint64_t g_purgeable;
static _Atomic double g_purgeable_ts;  // CFAbsoluteTime of last refresh, 0 = never
static atomic_int g_purgeable_busy;
static const fio_key_t g_disk_keys[2] = {FIO_KEY("Bytes (Read)"), FIO_KEY("Bytes (Write)")};

static void disk_init(void) {
    g_root_fd = open("/", O_RDONLY | O_CLOEXEC);
    struct statfs st;
    if (fstatfs(g_root_fd, &st) != 0) return;
    const char *dev = st.f_mntfromname;
    if (!strncmp(dev, "/dev/", 5)) dev += 5;
    io_registry_entry_t e = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, dev));
    for (int depth = 0; e && depth < 64; depth++) {
        if (IOObjectConformsTo(e, "IOBlockStorageDriver")) { g_disk_driver = e; return; }
        io_registry_entry_t p = 0;
        kern_return_t kr = IORegistryEntryGetParentEntry(e, kIOServicePlane, &p);
        IOObjectRelease(e);
        e = kr == KERN_SUCCESS ? p : 0;
    }
    if (e) IOObjectRelease(e);
}

void sampler_refresh_purgeable(double max_age_s) {
    double now = CFAbsoluteTimeGetCurrent();
    if (now - atomic_load(&g_purgeable_ts) < max_age_s) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_purgeable_busy, &expected, 1)) return;
    // Slow, mostly waiting work: keep it off the main thread.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, CFSTR("/"), kCFURLPOSIXPathStyle, true);
        uint64_t v = CSDiskSpaceGetRecoveryEstimate(url);
        CFRelease(url);
        atomic_store(&g_purgeable, v);
        atomic_store(&g_purgeable_ts, CFAbsoluteTimeGetCurrent());
        atomic_store(&g_purgeable_busy, 0);
    });
}

static void disk_space(snapshot_t *s) {
    struct statfs st;
    if (g_root_fd < 0 || fstatfs(g_root_fd, &st) != 0) return;
    uint64_t total = (uint64_t)st.f_blocks * st.f_bsize, avail = (uint64_t)st.f_bavail * st.f_bsize;
    uint64_t purge = atomic_load(&g_purgeable);
    uint64_t used = total - avail;
    if (purge > used) purge = 0;
    s->disk_total = total;
    s->disk_purgeable = purge;
    s->disk_free = avail + purge;  // matches Finder/original Stats "available"
}

// Rates use the real time between counter reads; the nominal interval can be wrong.
static double since(uint64_t prev, uint64_t now) {
    static double tick_s;
    if (!tick_s) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); tick_s = (double)tb.numer / tb.denom / 1e9; }
    return prev && now > prev ? (double)(now - prev) * tick_s : 0;
}

static void disk_io(snapshot_t *s, double unused_dt) {
    (void)unused_dt;
    int64_t v[2] = {-1, -1};
    if (!g_disk_driver || fio_read_numbers(g_disk_driver, "Statistics", g_disk_keys, 2, v) < 2) return;
    uint64_t now = mach_absolute_time();
    double dt = since(g_disk_prev_t, now);
    g_disk_prev_t = now;
    s->disk_read_bps = s->disk_write_bps = 0;
    if (g_disk_prev_r >= 0 && dt > 0) {
        s->disk_read_bps = v[0] >= g_disk_prev_r ? (double)(v[0] - g_disk_prev_r) / dt : 0;
        s->disk_write_bps = v[1] >= g_disk_prev_w ? (double)(v[1] - g_disk_prev_w) / dt : 0;
    }
    g_disk_prev_r = v[0];
    g_disk_prev_w = v[1];
}

// ---- Network -------------------------------------------------------------------
static unsigned g_net_ifindex;
static char g_net_ifname[16];
static uint64_t g_net_prev_in, g_net_prev_out;
static int g_net_primed, g_net_stale = 1;
static uint64_t g_net_prev_t;

void sampler_net_invalidate(void) { g_net_stale = 1; }

void sampler_reprime_rates(void) {  // after a gap in sampling: next rate read is a baseline
    g_net_primed = 0;
    g_disk_prev_r = g_disk_prev_w = -1;
}

static void net_resolve(void) {
    g_net_stale = 0;
    g_net_ifindex = 0;
    g_net_ifname[0] = 0;
    g_net_primed = 0;
    CFDictionaryRef d = SCDynamicStoreCopyValue(NULL, CFSTR("State:/Network/Global/IPv4"));
    if (!d) d = SCDynamicStoreCopyValue(NULL, CFSTR("State:/Network/Global/IPv6"));  // IPv6-only networks
    if (!d) return;
    CFStringRef name = CFGetTypeID(d) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(d, CFSTR("PrimaryInterface")) : NULL;
    if (name && CFGetTypeID(name) == CFStringGetTypeID() &&
        CFStringGetCString(name, g_net_ifname, sizeof(g_net_ifname), kCFStringEncodingUTF8))
        g_net_ifindex = if_nametoindex(g_net_ifname);
    CFRelease(d);
}

static void net(snapshot_t *s, double unused_dt) {
    (void)unused_dt;
    if (g_net_stale) net_resolve();
    s->net_ok = 0;
    s->net_in_bps = s->net_out_bps = 0;  // offline or priming: no stale rates on screen
    if (!g_net_ifindex) { s->net_in_total = s->net_out_total = 0; s->net_if[0] = 0; return; }
    int mib[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, (int)g_net_ifindex, IFDATA_GENERAL};
    struct ifmibdata md;
    size_t len = sizeof(md);
    if (sysctl(mib, 6, &md, &len, NULL, 0) != 0) { g_net_stale = 1; return; }
    uint64_t in = md.ifmd_data.ifi_ibytes, out = md.ifmd_data.ifi_obytes;
    uint64_t now = mach_absolute_time();
    double dt = since(g_net_prev_t, now);
    g_net_prev_t = now;
    if (g_net_primed && dt > 0) {
        s->net_in_bps = in >= g_net_prev_in ? (double)(in - g_net_prev_in) / dt : 0;
        s->net_out_bps = out >= g_net_prev_out ? (double)(out - g_net_prev_out) / dt : 0;
    }
    g_net_prev_in = in;
    g_net_prev_out = out;
    g_net_primed = 1;
    s->net_ok = 1;
    s->net_in_total = in;
    s->net_out_total = out;
    memcpy(s->net_if, g_net_ifname, sizeof(s->net_if));
}

// ---- Battery ---------------------------------------------------------------------
void sampler_battery(snapshot_t *s) {
    s->batt_present = 0;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (!d) continue;
        CFStringRef type = CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
        int cur = 0, max = 100, tte = -1, ttf = -1;
        CFNumberRef n;
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey)))) CFNumberGetValue(n, kCFNumberIntType, &cur);
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey)))) CFNumberGetValue(n, kCFNumberIntType, &max);
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSTimeToEmptyKey)))) CFNumberGetValue(n, kCFNumberIntType, &tte);
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSTimeToFullChargeKey)))) CFNumberGetValue(n, kCFNumberIntType, &ttf);
        CFBooleanRef b = CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey));
        CFStringRef st = CFDictionaryGetValue(d, CFSTR(kIOPSPowerSourceStateKey));
        s->batt_present = 1;
        s->batt_level = max > 0 ? (float)cur / (float)max : 0;
        s->batt_charging = b && CFBooleanGetValue(b);
        s->batt_ac = st && CFStringCompare(st, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo;
        s->batt_minutes = s->batt_charging ? ttf : (s->batt_ac ? -1 : tte);
        break;
    }
    if (list) CFRelease(list);
    CFRelease(info);
}

// ---- Accessory batteries --------------------------------------------------------------
static void bt_add(snapshot_t *s, const char *name, float level, int charging) {
    for (int i = 0; i < s->nbt; i++)
        if (!strcmp(s->bt[i].name, name)) return;  // same device from two sources
    if (s->nbt >= 8 || level < 0 || level > 1) return;
    strlcpy(s->bt[s->nbt].name, name, sizeof s->bt[0].name);
    s->bt[s->nbt].level = level;
    s->bt[s->nbt].charging = charging;
    s->nbt++;
}

void sampler_bluetooth(snapshot_t *s) {
    s->nbt = 0;
    // 1) IOPS accessory power sources (what `pmset -g accps` lists).
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = info ? IOPSCopyPowerSourcesList(info) : NULL;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (!d) continue;
        CFStringRef type = CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) == kCFCompareEqualTo ||
            CFStringCompare(type, CFSTR(kIOPSUPSType), 0) == kCFCompareEqualTo) continue;
        CFStringRef nm = CFDictionaryGetValue(d, CFSTR(kIOPSNameKey));
        CFNumberRef cur = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey)), mx = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey));
        int c = -1, m = 100;
        if (cur) CFNumberGetValue(cur, kCFNumberIntType, &c);
        if (mx) CFNumberGetValue(mx, kCFNumberIntType, &m);
        CFBooleanRef ch = CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey));
        char name[40] = "Accessory";
        if (nm) CFStringGetCString(nm, name, sizeof name, kCFStringEncodingUTF8);
        if (c >= 0 && m > 0) bt_add(s, name, (float)c / (float)m, ch && CFBooleanGetValue(ch));
    }
    if (list) CFRelease(list);
    if (info) CFRelease(info);
    // 2) Apple HID devices that report BatteryPercent.
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleDeviceManagementHIDEventService"), &it) != KERN_SUCCESS) return;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        CFTypeRef pct = IORegistryEntryCreateCFProperty(o, CFSTR("BatteryPercent"), NULL, 0);
        CFTypeRef prod = IORegistryEntryCreateCFProperty(o, CFSTR("Product"), NULL, 0);
        int v = -1;
        if (pct && CFGetTypeID(pct) == CFNumberGetTypeID()) CFNumberGetValue(pct, kCFNumberIntType, &v);
        char name[40] = "Bluetooth device";
        if (prod && CFGetTypeID(prod) == CFStringGetTypeID()) CFStringGetCString(prod, name, sizeof name, kCFStringEncodingUTF8);
        if (v >= 0) bt_add(s, name, (float)v / 100.0f, 0);
        if (pct) CFRelease(pct);
        if (prod) CFRelease(prod);
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
}

// ---- Entry points -------------------------------------------------------------------
void sampler_init(void) {
    g_host = mach_host_self();
    host_page_size(g_host, &g_page);
    size_t sz = sizeof(g_memsize);
    sysctlbyname("hw.memsize", &g_memsize, &sz, NULL, 0);
    size_t ml = 4;
    if (sysctlnametomib("kern.memorystatus_vm_pressure_level", g_mib_pressure, &ml) == 0) g_mib_pressure_len = (u_int)ml;
    ml = 4;
    if (sysctlnametomib("vm.swapusage", g_mib_swap, &ml) == 0) g_mib_swap_len = (u_int)ml;
    cpu_topology_init();
    g_gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    disk_init();
    // Prime accumulators so the first visible sample is a real delta.
    snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    cpu_total(&tmp);
    gpu(&tmp);
}

void sampler_collect(unsigned need, double dt, snapshot_t *s) {
    if (need & NEED_CPU) cpu_total(s);
    if (need & NEED_CPU_CORES) cpu_cores(s);
    else g_cores_primed = 0;
    if (need & NEED_LOAD) getloadavg(s->load, 3);
    if (need & (NEED_MEM | NEED_MEM_DETAIL)) mem(s, (need & NEED_MEM_DETAIL) != 0);
    if (need & NEED_GPU) gpu(s);
    if (need & NEED_DISK_SPACE) disk_space(s);
    if (need & NEED_DISK_IO) disk_io(s, dt);
    else g_disk_prev_r = -1;
    if (need & NEED_NET) net(s, dt);
    else g_net_primed = 0;
    if (need & NEED_SENSORS) smc_sensors(s);
}
