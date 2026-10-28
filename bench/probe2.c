// probe2: IORegistryEntryCreateCFProperty vs calling the MIG routine
// io_registry_entry_get_property_bin_buf directly and scanning the reply for one key.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_mib.h>


// Hand-rolled MIG client for iokit routine 2889 (io_registry_entry_get_property_bin_buf).
// The stub exists in IOKit but is not exported; layouts were read from its disassembly.
#include <mach/mig.h>
#include <mach/ndr.h>
typedef struct {
    mach_msg_header_t hdr;
    NDR_record_t ndr;
    uint8_t var[2 * (8 + 128) + 4 + 8 + 8];
} req2889_t;
typedef struct {
    mach_msg_header_t hdr;            // 0x00
    mach_msg_body_t body;             // 0x18
    mach_msg_ool_descriptor64_t ool;  // 0x1c (16 bytes)
    NDR_record_t ndr;                 // 0x2c
    uint64_t bufsize;                 // 0x34
    uint32_t cnt;                     // 0x3c
    uint8_t trailer[64];
} __attribute__((packed)) rep2889_t;
extern mach_msg_return_t mach_msg2_internal(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static size_t put_cstr(uint8_t *p, const char *s) {
    size_t n = strlen(s) + 1;              // MIG c_string count includes NUL
    uint32_t off = 0, cnt = (uint32_t)n;
    memcpy(p, &off, 4); memcpy(p + 4, &cnt, 4);
    memset(p + 8, 0, (n + 3) & ~3u);
    memcpy(p + 8, s, n);
    return 8 + ((n + 3) & ~3u);
}
static kern_return_t io_registry_entry_get_property_bin_buf(
    mach_port_t entry, const char *plane, const char *prop, uint32_t options,
    mach_vm_address_t buf, mach_vm_size_t *bufsize, vm_offset_t *properties, mach_msg_type_number_t *cnt) {
    union { req2889_t req; rep2889_t rep; } m;
    uint8_t *p = m.req.var;
    p += put_cstr(p, plane);
    p += put_cstr(p, prop);
    memcpy(p, &options, 4); p += 4;
    memcpy(p, &buf, 8); p += 8;
    memcpy(p, bufsize, 8); p += 8;
    mach_port_t reply = mig_get_reply_port();
    mach_msg_size_t sz = (mach_msg_size_t)(p - (uint8_t *)&m.req);
    // Same packing IOKit's stub uses: SEND|RCV|KOBJECT_CALL, header fields packed in pairs.
    kern_return_t kr = mach_msg2_internal(&m, 0x3ull | (0x2ull << 32),
        (uint64_t)MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | ((uint64_t)sz << 32),
        (uint64_t)entry | ((uint64_t)reply << 32),
        (uint64_t)2889 << 32,
        (uint64_t)reply << 32,
        sizeof(m.rep),
        0);
    if (kr != KERN_SUCCESS) {
        if (kr == MACH_SEND_INVALID_REPLY || kr == MACH_RCV_PORT_DIED || kr == MACH_RCV_INTERRUPTED) mig_dealloc_reply_port(reply);
        else mig_put_reply_port(reply);
        return kr;
    }
    if (m.rep.hdr.msgh_id != 2989) { mach_msg_destroy(&m.rep.hdr); return MIG_REPLY_MISMATCH; }
    if (!(m.rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {   // simple reply = error code
        kern_return_t rc; memcpy(&rc, (uint8_t *)&m.rep + 0x20, 4);
        return rc ? rc : MIG_TYPE_ERROR;
    }
    if (m.rep.body.msgh_descriptor_count != 1 || m.rep.ool.type != MACH_MSG_OOL_DESCRIPTOR ||
        m.rep.ool.size != m.rep.cnt) { mach_msg_destroy(&m.rep.hdr); return MIG_TYPE_ERROR; }
    *bufsize = m.rep.bufsize;
    *properties = (vm_offset_t)m.rep.ool.address;
    *cnt = m.rep.cnt;
    return KERN_SUCCESS;
}

static mach_timebase_info_data_t tb;
static uint64_t instr_now(void) {
    struct rusage_info_v6 ru;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru);
    return ru.ri_instructions;
}
#define BENCH(name, iters, body)                                                         \
    do {                                                                                 \
        for (int w = 0; w < 3; w++) { body; }                                            \
        uint64_t i0 = instr_now(), t0 = mach_absolute_time();                            \
        for (int it = 0; it < (iters); it++) { body; }                                   \
        uint64_t t1 = mach_absolute_time(), i1 = instr_now();                            \
        printf("%-52s %9.0f ns/call %10.0f instr/call\n", name,                          \
               (double)(t1 - t0) * tb.numer / tb.denom / (iters), (double)(i1 - i0) / (iters)); \
    } while (0)

// OSSerializeBinary: little-endian 32-bit words. Signature 0xd3, then objects:
// word = [end:1][type:7][len:24]. Number: len=bits, followed by u64.
// Symbol/String/Data: len bytes, padded to 4. Dictionary: alternating key, value.
enum { kDict = 0x01, kArray = 0x02, kSet = 0x03, kNumber = 0x04, kSymbol = 0x08, kString = 0x09,
       kData = 0x0a, kBool = 0x0b, kObject = 0x0c };

// The u64 number that follows symbol `key` at the top level, or -1 when absent
// (PerformanceStatistics and Statistics are flat dictionaries of numbers).
static int64_t osbin_find_number(const uint32_t *w, size_t nwords, const char *key, size_t keylen) {
    if (nwords < 2 || w[0] != 0xd3) return -1;
    size_t i = 1;
    int want = 0;
    while (i < nwords) {
        uint32_t h = w[i++];
        uint32_t type = (h >> 24) & 0x7f, len = h & 0xffffff;
        switch (type) {
        case kDict: case kArray: case kSet: want = 0; break;
        case kNumber:
            if (want) return (int64_t)((uint64_t)w[i] | (uint64_t)w[i + 1] << 32);
            i += 2; want = 0; break;
        case kSymbol: case kString: {
            const char *s = (const char *)&w[i];
            // symbols include the NUL terminator in len
            want = (type == kSymbol && len == keylen + 1 && memcmp(s, key, keylen) == 0);
            i += (len + 3) / 4;
            break;
        }
        case kData: i += (len + 3) / 4; want = 0; break;
        case kBool: case kObject: want = 0; break;
        default: return -1;
        }
    }
    return -1;
}

static int64_t cf_number(CFDictionaryRef d, CFStringRef k) {
    CFNumberRef n = CFDictionaryGetValue(d, k);
    int64_t v = -1;
    if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mach_timebase_info(&tb);
    io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));

    static uint32_t buf[16384];
    int64_t v1 = -1, v2 = -1;
    BENCH("GPU: CreateCFProperty + CFDictionaryGetValue", 5000, {
        CFDictionaryRef p = IORegistryEntryCreateCFProperty(gpu, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        v1 = cf_number(p, CFSTR("Device Utilization %"));
        CFRelease(p);
    });
    BENCH("GPU: get_property_bin_buf + scan", 5000, {
        mach_vm_size_t sz = sizeof(buf);
        vm_offset_t ool = 0; mach_msg_type_number_t oolcnt = 0;
        kern_return_t kr = io_registry_entry_get_property_bin_buf(gpu, kIOServicePlane, "PerformanceStatistics", 0,
                                                                  (mach_vm_address_t)buf, &sz, &ool, &oolcnt);
        if (kr == KERN_SUCCESS && !oolcnt) v2 = osbin_find_number(buf, sz / 4, "Device Utilization %", 20);
        if (oolcnt) vm_deallocate(mach_task_self(), ool, oolcnt);
    });
    printf("  gpu util: cf=%lld bin=%lld\n", v1, v2);
    {
        mach_vm_size_t sz = sizeof(buf); vm_offset_t ool = 0; mach_msg_type_number_t c = 0;
        io_registry_entry_get_property_bin_buf(gpu, kIOServicePlane, "PerformanceStatistics", 0, (mach_vm_address_t)buf, &sz, &ool, &c);
        printf("  serialized PerformanceStatistics: %llu bytes (ool=%u)\n", sz, c);
    }

    // Disk statistics on the root disk driver
    struct statfs st; statfs("/", &st);
    const char *dev = st.f_mntfromname + 5;
    io_registry_entry_t e = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, dev));
    while (e && !IOObjectConformsTo(e, "IOBlockStorageDriver")) {
        io_registry_entry_t p = 0; IORegistryEntryGetParentEntry(e, kIOServicePlane, &p); IOObjectRelease(e); e = p;
    }
    BENCH("Disk: CreateCFProperty(Statistics) + get", 5000, {
        CFDictionaryRef p = IORegistryEntryCreateCFProperty(e, CFSTR("Statistics"), kCFAllocatorDefault, 0);
        v1 = cf_number(p, CFSTR("Bytes (Read)"));
        CFRelease(p);
    });
    BENCH("Disk: get_property_bin_buf + scan", 5000, {
        mach_vm_size_t sz = sizeof(buf); vm_offset_t ool = 0; mach_msg_type_number_t c = 0;
        if (io_registry_entry_get_property_bin_buf(e, kIOServicePlane, "Statistics", 0, (mach_vm_address_t)buf, &sz, &ool, &c) == KERN_SUCCESS && !c)
            v2 = osbin_find_number(buf, sz / 4, "Bytes (Read)", 12);
        if (c) vm_deallocate(mach_task_self(), ool, c);
    });
    printf("  disk bytes read: cf=%lld bin=%lld\n", v1, v2);

    // Network: per-interface ifmib vs NET_RT_IFLIST2
    unsigned idx = if_nametoindex("en0");
    int mib[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, (int)idx, IFDATA_GENERAL};
    struct ifmibdata md;
    BENCH("Net: sysctl IFMIB_IFDATA (en0)", 20000, { size_t l = sizeof(md); sysctl(mib, 6, &md, &l, NULL, 0); });
    printf("  en0 ibytes=%llu obytes=%llu\n", md.ifmd_data.ifi_ibytes, md.ifmd_data.ifi_obytes);

    // Disk space
    BENCH("Disk: statfs(/)", 20000, { statfs("/", &st); });
    BENCH("Disk: getattrlist-free fstatfs(fd)", 20000, { static int fd = -1; if (fd < 0) fd = open("/", O_RDONLY); fstatfs(fd, &st); });
    return 0;
}
