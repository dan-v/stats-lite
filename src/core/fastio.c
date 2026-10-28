#include "fastio.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mig.h>
#include <mach/ndr.h>
#include <string.h>

// Exported by libsystem_kernel; this is what MIG stubs call on current macOS.
extern mach_msg_return_t mach_msg2_internal(void *data, uint64_t option64, uint64_t bits_and_send_size,
                                            uint64_t remote_and_local, uint64_t voucher_and_id,
                                            uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority,
                                            uint64_t timeout);

#define IOKIT_GET_PROPERTY_BIN_BUF 2889
#define MACH64_SEND_MSG_ 0x1ull
#define MACH64_RCV_MSG_ 0x2ull
#define MACH64_SEND_KOBJECT_CALL_ 0x200000000ull  // required for kernel-object ports

typedef struct {
    mach_msg_header_t hdr;
    NDR_record_t ndr;
    // plane c_string, property c_string (offset, count, data[<=128]), options, buf, bufsize
    uint8_t var[2 * (8 + 128) + 4 + 8 + 8];
} req_t;

typedef struct __attribute__((packed)) {
    mach_msg_header_t hdr;            // 0x00
    mach_msg_body_t body;             // 0x18
    mach_msg_ool_descriptor64_t ool;  // 0x1c
    NDR_record_t ndr;                 // 0x2c
    uint64_t bufsize;                 // 0x34
    uint32_t cnt;                     // 0x3c
    uint8_t trailer[68];              // room for the max receive trailer
} rep_t;

static int g_fast_ok = 1;
static uint32_t g_buf[1024];  // 4 KB; PerformanceStatistics is ~416 bytes

int fio_fast_path_active(void) { return g_fast_ok; }

static size_t put_cstr(uint8_t *p, const char *s) {
    size_t n = strlen(s) + 1;  // MIG c_string count includes the NUL
    if (n > 128) n = 128;
    uint32_t off = 0, cnt = (uint32_t)n;
    size_t padded = (n + 3) & ~(size_t)3;
    memcpy(p, &off, 4);
    memcpy(p + 4, &cnt, 4);
    memset(p + 8, 0, padded);
    memcpy(p + 8, s, n - 1);
    return 8 + padded;
}

// Returns pointer to serialized data and its size, or NULL. *ool is set when the
// kernel returned out-of-line memory that the caller must vm_deallocate.
static const uint32_t *get_bin(io_registry_entry_t entry, const char *prop, size_t *nbytes, vm_offset_t *ool,
                               mach_msg_type_number_t *oolcnt) {
    union {
        req_t req;
        rep_t rep;
    } m;
    uint8_t *p = m.req.var;
    p += put_cstr(p, kIOServicePlane);
    p += put_cstr(p, prop);
    uint32_t options = 0;
    uint64_t buf = (uint64_t)(uintptr_t)g_buf, bufsize = sizeof(g_buf);
    memcpy(p, &options, 4); p += 4;
    memcpy(p, &buf, 8); p += 8;
    memcpy(p, &bufsize, 8); p += 8;
    m.req.ndr = NDR_record;
    uint32_t size = (uint32_t)(p - (uint8_t *)&m.req);

    mach_port_t reply = mig_get_reply_port();
    mach_msg_return_t kr = mach_msg2_internal(
        &m, MACH64_SEND_MSG_ | MACH64_RCV_MSG_ | MACH64_SEND_KOBJECT_CALL_,
        (uint64_t)MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | ((uint64_t)size << 32),
        (uint64_t)entry | ((uint64_t)reply << 32), (uint64_t)IOKIT_GET_PROPERTY_BIN_BUF << 32,
        (uint64_t)reply << 32, sizeof(m.rep), 0);
    if (kr != MACH_MSG_SUCCESS) {
        // Same reply-port policy as IOKit's generated stub: keep the port only for
        // send errors that never reached the reply port, otherwise discard it.
        uint32_t d = (uint32_t)kr - (uint32_t)MACH_SEND_INVALID_DATA;
        if (d <= 14 && ((1u << d) & 0x4003u)) mig_put_reply_port(reply);
        else mig_dealloc_reply_port(reply);
        return NULL;
    }
    if (m.rep.hdr.msgh_id != IOKIT_GET_PROPERTY_BIN_BUF + 100) {  // reply mismatch / server died
        mach_msg_destroy(&m.rep.hdr);
        if (m.rep.hdr.msgh_id != MACH_NOTIFY_SEND_ONCE) g_fast_ok = 0;
        return NULL;
    }
    if (!(m.rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        // Simple reply: exactly header + NDR + RetCode, carrying a non-zero error
        // (e.g. property not found). Anything else is a layout we don't know.
        kern_return_t rc;
        memcpy(&rc, (uint8_t *)&m.rep + 0x20, 4);
        if (m.rep.hdr.msgh_size != 0x24 || m.rep.hdr.msgh_remote_port != MACH_PORT_NULL || rc == KERN_SUCCESS) g_fast_ok = 0;
        return NULL;
    }
    if (m.rep.hdr.msgh_size != 0x40 || m.rep.hdr.msgh_remote_port != MACH_PORT_NULL ||
        m.rep.body.msgh_descriptor_count != 1 || m.rep.ool.type != MACH_MSG_OOL_DESCRIPTOR || m.rep.ool.size != m.rep.cnt) {
        mach_msg_destroy(&m.rep.hdr);
        g_fast_ok = 0;  // the ABI changed: use the public API from now on
        return NULL;
    }
    *oolcnt = m.rep.cnt;
    *ool = (vm_offset_t)m.rep.ool.address;
    if (m.rep.cnt) {  // didn't fit in our buffer: data is out-of-line
        *nbytes = m.rep.cnt;
        return (const uint32_t *)*ool;
    }
    if (m.rep.bufsize > sizeof(g_buf)) { g_fast_ok = 0; return NULL; }
    *nbytes = (size_t)m.rep.bufsize;
    return g_buf;
}

enum { kDict = 1, kArray = 2, kSet = 3, kNumber = 4, kSymbol = 8, kString = 9, kData = 10, kBool = 11, kObject = 12 };

// Parses OSSerializeBinary like libkern does (a set top bit marks the last object of
// its collection) and extracts numbers for keys at the top level of the root dict.
// Returns -1 when the stream is malformed or a wanted value is an object reference.
int fio_scan(const uint32_t *w, size_t n, const fio_key_t *keys, int nkeys, int64_t *out) {
    if (n < 2 || w[0] != 0x000000d3u) return -1;
    uint8_t ends[32];  // per open collection: did its header carry the end bit
    int depth = 0, found = 0, want = -1, key_refs = 0;
    uint32_t top_items = 0;
    size_t i = 1;
    // Root object: must be a dictionary.
    uint32_t h = w[i++];
    if (((h >> 24) & 0x7f) != kDict) return -1;
    if ((h & 0xffffff) == 0) return 0;
    ends[depth++] = 1;
    while (depth > 0) {
        if (i >= n) return -1;
        h = w[i++];
        uint32_t type = (h >> 24) & 0x7f, len = h & 0xffffff, end = h >> 31;
        int top = depth == 1, is_key = top && (top_items % 2 == 0), is_val = top && !is_key;
        if (top) top_items++;
        int opens = 0;
        switch (type) {
        case kNumber:
            if (i + 2 > n) return -1;
            if (is_val && want >= 0) {
                out[want] = (int64_t)((uint64_t)w[i] | (uint64_t)w[i + 1] << 32);
                found++;
            }
            i += 2;
            break;
        case kSymbol: case kString: case kData: {
            size_t words = (len + 3) / 4;
            if (i + words > n) return -1;
            if (is_key) {
                want = -1;
                if (type == kSymbol)
                    for (int k = 0; k < nkeys; k++)
                        if (len == keys[k].len + 1 && memcmp(&w[i], keys[k].name, keys[k].len) == 0) { want = k; break; }
            }
            i += words;
            break;
        }
        case kBool: break;
        case kObject:
            if (is_val && want >= 0) return -1;  // value shared by reference: let CF resolve it
            if (is_key) { key_refs = 1; want = -1; }
            break;
        case kDict: case kArray: case kSet:
            if (len) {
                if (depth >= (int)sizeof(ends)) return -1;
                ends[depth++] = (uint8_t)end;
                opens = 1;
            }
            break;
        default: return -1;
        }
        if (is_val) want = -1;
        if (!opens && end) {  // closes the current collection, and any ancestors it was last in
            while (depth > 0) {
                uint8_t parent_end = ends[--depth];
                if (!parent_end) break;
            }
        }
    }
    if (found < nkeys && key_refs) return -1;  // a wanted key may hide behind a reference
    return found;
}

static int cf_read_numbers(io_registry_entry_t entry, const char *prop, const fio_key_t *keys, int nkeys, int64_t *out) {
    CFStringRef p = CFStringCreateWithCString(NULL, prop, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, p, kCFAllocatorDefault, 0);
    CFRelease(p);
    if (!v) return -1;
    int found = 0;
    if (CFGetTypeID(v) == CFDictionaryGetTypeID()) {
        for (int k = 0; k < nkeys; k++) {
            CFStringRef ks = CFStringCreateWithCString(NULL, keys[k].name, kCFStringEncodingUTF8);
            CFTypeRef num = CFDictionaryGetValue(v, ks);
            CFRelease(ks);
            if (num && CFGetTypeID(num) == CFNumberGetTypeID() && CFNumberGetValue(num, kCFNumberSInt64Type, &out[k])) found++;
        }
    }
    CFRelease(v);
    return found;
}

int fio_read_numbers(io_registry_entry_t entry, const char *prop, const fio_key_t *keys, int nkeys, int64_t *out) {
    if (!entry) return -1;
    if (g_fast_ok) {
        size_t nbytes = 0;
        vm_offset_t ool = 0;
        mach_msg_type_number_t oolcnt = 0;
        const uint32_t *data = get_bin(entry, prop, &nbytes, &ool, &oolcnt);
        if (data) {
            int r = fio_scan(data, nbytes / 4, keys, nkeys, out);
            if (oolcnt) vm_deallocate(mach_task_self(), ool, oolcnt);
            if (r >= 0) return r;
            g_fast_ok = 0;  // unknown stream format: never try again
        } else if (g_fast_ok) {
            return -1;  // property missing or transient error; CF would fail too
        }
    }
    return cf_read_numbers(entry, prop, keys, nkeys, out);
}
