#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remote_auth.h"
#include "storage.h"

#include "mbedtls/sha256.h"

#define M65_SIG_BLOCK_SIZE 256u

typedef enum {
    M65_SIGNED_FILE_ANY = 0,
    M65_SIGNED_FILE_BIT = 1,
    M65_SIGNED_FILE_COR = 2,
    M65_SIGNED_FILE_M65J = 3,
} m65_signed_file_type_t;

typedef struct {
    char final_path[256];
    char tmp_path[272];
    const remote_auth_config_t *cfg;
    uint32_t total_length;
    uint32_t block_offset;
    uint32_t done;
    uint32_t payload_done;
    bool require_signature;
    bool candidate_signature;
    bool saw_magic;
    bool rejected;
    m65_signed_file_type_t file_type;
    uint8_t block[M65_SIG_BLOCK_SIZE];
    uint32_t block_done;
    mbedtls_sha256_context sha;
    storage_file_t out_file;
    bool out_open;
} signed_file_rx_t;

const char *signed_file_last_error(void);
const char *signed_file_type_name(m65_signed_file_type_t type);
m65_signed_file_type_t signed_file_type_from_path(const char *path);

bool signed_file_receive_begin(signed_file_rx_t *rx,
                               const remote_auth_config_t *cfg,
                               const char *final_path,
                               const char *tmp_path,
                               uint32_t total_length,
                               m65_signed_file_type_t file_type);
bool signed_file_receive_write(signed_file_rx_t *rx, const uint8_t *data, size_t len);
bool signed_file_receive_finish(signed_file_rx_t *rx);
void signed_file_receive_abort(signed_file_rx_t *rx);
