// fastio: reads numbers from flat IORegistry dictionaries without CF objects.
// Sends the MIG request io_registry_entry_get_property_bin_buf (iokit routine 2889)
// directly and scans the OSSerializeBinary reply in a static buffer. If a reply
// ever fails validation we permanently fall back to the public CF API.
#pragma once
#include <IOKit/IOKitLib.h>
#include <stdint.h>

typedef struct {
    const char *name;  // key as it appears in the dictionary
    uint32_t len;      // strlen(name)
} fio_key_t;

#define FIO_KEY(s) {s, sizeof(s) - 1}

// Reads `prop` from `entry` and stores the numeric value of each key into out[i]
// (left untouched when missing). Returns the number of keys found, -1 on error.
int fio_read_numbers(io_registry_entry_t entry, const char *prop, const fio_key_t *keys, int nkeys, int64_t *out);

// Scans an OSSerializeBinary stream. Exposed for tests.
int fio_scan(const uint32_t *words, size_t nwords, const fio_key_t *keys, int nkeys, int64_t *out);

// 1 when the hand-rolled MIG path is active, 0 after falling back to CF.
int fio_fast_path_active(void);
