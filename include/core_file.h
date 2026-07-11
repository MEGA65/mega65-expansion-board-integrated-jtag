#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage.h"

typedef enum {
    CORE_KIND_UNKNOWN = 0,
    CORE_KIND_BIT,
    CORE_KIND_COR,
    CORE_KIND_RAW_M65J,
} core_kind_t;

typedef struct {
    char path[256];
    storage_file_t file;
    core_kind_t kind;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint32_t expected_idcode;
} core_file_t;

bool core_open(core_file_t *cf, const char *path);
void core_close(core_file_t *cf);
const char *core_last_error(void);

bool core_rewind_payload(core_file_t *cf);
bool core_read_payload(core_file_t *cf, uint8_t *buf, size_t len, size_t *got);
const char *core_kind_name(core_kind_t kind);
