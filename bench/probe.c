// probe: per-call cost (median ns, instructions) of candidate kernel/IOKit APIs per metric.
//
//   probe            run all API cost probes
//   probe smckeys    dump SMC temperature/power/fan keys with values

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sysctl.h>

static mach_timebase_info_data_t tb;
static uint64_t instr_now(void) {
    struct rusage_info_v6 ru;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru);
    return ru.ri_instructions;
}

#define BENCH(name, iters, body)                                                         \
    do {                                                                                 \
        for (int w = 0; w < 3; w++) { body; }                                            \
        uint64_t i0 = instr_now();                                                       \
        uint64_t t0 = mach_absolute_time();                                              \
        for (int it = 0; it < (iters); it++) { body; }                                   \
        uint64_t t1 = mach_absolute_time();                                              \
        uint64_t i1 = instr_now();                                                       \
        double ns = (double)(t1 - t0) * tb.numer / tb.denom / (iters);                   \
        printf("%-48s %10.0f ns/call %12.0f instr/call\n", name, ns, (double)(i1 - i0) / (iters)); \
    } while (0)

// ---- SMC -------------------------------------------------------------------
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
_Static_assert(sizeof(smc_data_t) == 80, "SMC struct must be 80 bytes");

static io_connect_t smc;
static kern_return_t smc_call(smc_data_t *in, smc_data_t *out) {
    size_t osz = sizeof(*out);
    return IOConnectCallStructMethod(smc, 2, in, sizeof(*in), out, &osz);
}
static uint32_t fourcc(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3]; }
static int smc_info(uint32_t key, smc_keyinfo_t *info) {
    smc_data_t in = {0}, out = {0};
    in.key = key; in.data8 = 9;
    if (smc_call(&in, &out) != KERN_SUCCESS || out.result) return -1;
    *info = out.keyInfo;
    return 0;
}
static int smc_read(uint32_t key, smc_keyinfo_t info, uint8_t *bytes) {
    smc_data_t in = {0}, out = {0};
    in.key = key; in.keyInfo.dataSize = info.dataSize; in.data8 = 5;
    if (smc_call(&in, &out) != KERN_SUCCESS || out.result) return -1;
    memcpy(bytes, out.bytes, 32);
    return 0;
}
static double smc_decode(smc_keyinfo_t info, const uint8_t *b) {
    char t[5] = {info.dataType >> 24, info.dataType >> 16, info.dataType >> 8, info.dataType, 0};
    if (!strcmp(t, "flt ")) { float f; memcpy(&f, b, 4); return f; }
    if (!strcmp(t, "ui8 ")) return b[0];
    if (!strcmp(t, "ui16")) return b[0] << 8 | b[1];
    if (!strcmp(t, "ui32")) return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    if (!strcmp(t, "sp78")) return (int16_t)(b[0] << 8 | b[1]) / 256.0;
    if (!strcmp(t, "fpe2")) return (b[0] << 6) + (b[1] >> 2);
    return -9999;
}
static void smc_open(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    IOServiceOpen(s, mach_task_self(), 0, &smc);
    IOObjectRelease(s);
}

static int smckeys(void) {
    smc_open();
    smc_keyinfo_t ki; uint8_t b[32];
    smc_info(fourcc("#KEY"), &ki); smc_read(fourcc("#KEY"), ki, b);
    uint32_t n = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    printf("%u keys\n", n);
    for (uint32_t i = 0; i < n; i++) {
        smc_data_t in = {0}, out = {0};
        in.data8 = 8; in.data32 = i;
        if (smc_call(&in, &out) != KERN_SUCCESS) continue;
        uint32_t k = out.key;
        char name[5] = {k >> 24, k >> 16, k >> 8, k, 0};
        if (name[0] != 'T' && name[0] != 'P' && name[0] != 'F') continue;
        if (smc_info(k, &ki) || smc_read(k, ki, b)) continue;
        char t[5] = {ki.dataType >> 24, ki.dataType >> 16, ki.dataType >> 8, ki.dataType, 0};
        printf("%s %s %8.2f\n", name, t, smc_decode(ki, b));
    }
    return 0;
}

// ---- helpers ----------------------------------------------------------------
static io_registry_entry_t find_root_disk_driver(void) {
    // Root volume BSD name -> IOMedia -> parent chain until IOBlockStorageDriver.
    struct statfs st; statfs("/", &st);
    const char *dev = st.f_mntfromname; // /dev/diskXsYsZ
    if (!strncmp(dev, "/dev/", 5)) dev += 5;
    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, dev));
    io_registry_entry_t e = media;
    while (e) {
        if (IOObjectConformsTo(e, "IOBlockStorageDriver")) return e;
        io_registry_entry_t p = 0;
        if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p) != KERN_SUCCESS) { IOObjectRelease(e); return 0; }
        IOObjectRelease(e);
        e = p;
    }
    return 0;
}

int main(int argc, char **argv) {
    mach_timebase_info(&tb);
    if (argc > 1 && !strcmp(argv[1], "smckeys")) return smckeys();
    host_t host = mach_host_self();

    BENCH("host_statistics(CPU_LOAD)", 20000, {
        host_cpu_load_info_data_t l; mach_msg_type_number_t c = HOST_CPU_LOAD_INFO_COUNT;
        host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&l, &c);
    });
    BENCH("host_processor_info + vm_deallocate", 20000, {
        natural_t n; processor_info_array_t a; mach_msg_type_number_t c;
        host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &n, &a, &c);
        vm_deallocate(mach_task_self(), (vm_address_t)a, c * sizeof(integer_t));
    });
    BENCH("host_statistics64(VM_INFO64)", 20000, {
        vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&v, &c);
    });
    BENCH("sysctlbyname(memorystatus_vm_pressure_level)", 20000, {
        int v; size_t s = sizeof(v); sysctlbyname("kern.memorystatus_vm_pressure_level", &v, &s, NULL, 0);
    });
    int pmib[4]; size_t pmiblen = 4;
    sysctlnametomib("kern.memorystatus_vm_pressure_level", pmib, &pmiblen);
    BENCH("sysctl(mib, pressure_level)", 20000, {
        int v; size_t s = sizeof(v); sysctl(pmib, (u_int)pmiblen, &v, &s, NULL, 0);
    });
    BENCH("sysctlbyname(vm.swapusage)", 20000, {
        struct xsw_usage x; size_t s = sizeof(x); sysctlbyname("vm.swapusage", &x, &s, NULL, 0);
    });
    BENCH("getloadavg", 20000, { double l[3]; getloadavg(l, 3); });

    BENCH("getifaddrs + walk + freeifaddrs", 5000, {
        struct ifaddrs *ifa; getifaddrs(&ifa);
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) (void)p->ifa_data;
        freeifaddrs(ifa);
    });
    unsigned idx = if_nametoindex("en0");
    int nmib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, (int)idx};
    char nbuf[4096];
    BENCH("sysctl(NET_RT_IFLIST2, en0 only)", 20000, {
        size_t len = sizeof(nbuf); sysctl(nmib, 6, nbuf, &len, NULL, 0);
    });
    int amib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
    static char abuf[65536];
    BENCH("sysctl(NET_RT_IFLIST2, all interfaces)", 5000, {
        size_t len = sizeof(abuf); sysctl(amib, 6, abuf, &len, NULL, 0);
    });
    BENCH("SCDynamicStoreCopyValue(primary iface)", 2000, {
        CFPropertyListRef v = SCDynamicStoreCopyValue(NULL, CFSTR("State:/Network/Global/IPv4"));
        if (v) CFRelease(v);
    });

    io_registry_entry_t drv = find_root_disk_driver();
    BENCH("IORegistryEntryCreateCFProperty(disk Statistics)", 5000, {
        CFTypeRef p = IORegistryEntryCreateCFProperty(drv, CFSTR("Statistics"), kCFAllocatorDefault, 0);
        if (p) CFRelease(p);
    });
    BENCH("statfs(/)", 20000, { struct statfs st; statfs("/", &st); });

    io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    BENCH("IORegistryEntryCreateCFProperty(GPU PerfStats)", 5000, {
        CFTypeRef p = IORegistryEntryCreateCFProperty(gpu, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (p) CFRelease(p);
    });
    BENCH("IORegistryEntryCreateCFProperties(GPU all)", 500, {
        CFMutableDictionaryRef p = NULL;
        IORegistryEntryCreateCFProperties(gpu, &p, kCFAllocatorDefault, 0);
        if (p) CFRelease(p);
    });
    BENCH("IOServiceGetMatchingServices(IOAccelerator)", 500, {
        io_iterator_t it; IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it);
        io_object_t o; while ((o = IOIteratorNext(it))) IOObjectRelease(o);
        IOObjectRelease(it);
    });

    smc_open();
    smc_keyinfo_t ki; uint8_t b[32];
    smc_info(fourcc("TB0T"), &ki);
    BENCH("SMC readKeyInfo", 5000, { smc_info(fourcc("PSTR"), &ki); });
    smc_info(fourcc("PSTR"), &ki);
    BENCH("SMC readBytes (cached keyinfo)", 5000, { smc_read(fourcc("PSTR"), ki, b); });

    BENCH("IOPSCopyPowerSourcesInfo+List+Desc", 2000, {
        CFTypeRef info = IOPSCopyPowerSourcesInfo();
        CFArrayRef list = IOPSCopyPowerSourcesList(info);
        if (list && CFArrayGetCount(list)) (void)IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, 0));
        if (list) CFRelease(list);
        CFRelease(info);
    });
    io_service_t batt = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    BENCH("IORegistryEntryCreateCFProperty(battery CurrentCapacity)", 5000, {
        CFTypeRef p = IORegistryEntryCreateCFProperty(batt, CFSTR("CurrentCapacity"), kCFAllocatorDefault, 0);
        if (p) CFRelease(p);
    });

    static pid_t pids[8192];
    int np = 0;
    BENCH("proc_listallpids", 2000, { np = proc_listallpids(pids, sizeof(pids)); });
    int okru = 0, okti = 0;
    BENCH("proc_pid_rusage(V2) over all pids", 50, {
        okru = 0;
        for (int i = 0; i < np; i++) { struct rusage_info_v2 r; okru += proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t *)&r) == 0; }
    });
    BENCH("proc_pidinfo(TASKINFO) over all pids", 50, {
        okti = 0;
        for (int i = 0; i < np; i++) { struct proc_taskinfo t; okti += proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &t, sizeof(t)) == sizeof(t); }
    });
    BENCH("proc_name over all pids", 50, {
        for (int i = 0; i < np; i++) { char n[64]; proc_name(pids[i], n, sizeof(n)); }
    });
    printf("processes=%d rusage_ok=%d taskinfo_ok=%d\n", np, okru, okti);
    return 0;
}
