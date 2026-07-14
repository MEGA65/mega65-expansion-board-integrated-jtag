#include "signed_file.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

static const uint8_t sig_magic[32] = {
    'M','6','5','J','T','A','G','-','S','I','G','B','L','O','C','K',
    '-','V','1',0xa5,0x65,0x19,0x83,0x42,0x7c,0xd1,0x5e,0x09,0xba,0x6f,0x34,0xc8
};

enum {
    SIG_OFF_MAGIC = 0,
    SIG_OFF_VERSION = 32,
    SIG_OFF_HEADER_LEN = 34,
    SIG_OFF_FLAGS = 36,
    SIG_OFF_PAYLOAD_LEN = 40,
    SIG_OFF_FILE_TYPE = 44,
    SIG_OFF_BOARD_ID = 45,
    SIG_OFF_HASH_ALG = 46,
    SIG_OFF_SIG_ALG = 47,
    SIG_OFF_KEY_ID = 48,
    SIG_OFF_PAYLOAD_SHA256 = 64,
    SIG_OFF_FILENAME = 96,
    SIG_FILENAME_LEN = 96,
    SIG_OFF_SIGNATURE = 192,
};

#define SIG_VERSION 1u
#define SIG_HASH_SHA256 1u
#define SIG_ALG_ECDSA_P256_SHA256 1u

static char last_err[160];

static void set_err(const char *msg)
{
    snprintf(last_err, sizeof last_err, "%s", msg);
}

const char *signed_file_last_error(void)
{
    return last_err[0] ? last_err : "no signed-file error";
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool is_zero16(const uint8_t *p)
{
    for (unsigned i = 0; i < 16; ++i) {
        if (p[i]) return false;
    }
    return true;
}

static size_t bounded_cstr_len(const char *s, size_t max_len)
{
    size_t n = 0;
    while (n < max_len && s[n]) n++;
    return n;
}

static bool sha256_starts(mbedtls_sha256_context *ctx)
{
    mbedtls_sha256_init(ctx);
    return mbedtls_sha256_starts(ctx, 0) == 0;
}

static bool sha256_update(mbedtls_sha256_context *ctx, const uint8_t *data, size_t len)
{
    return mbedtls_sha256_update(ctx, data, len) == 0;
}

static bool sha256_finish(mbedtls_sha256_context *ctx, uint8_t out[32])
{
    bool ok = mbedtls_sha256_finish(ctx, out) == 0;
    mbedtls_sha256_free(ctx);
    return ok;
}

static bool sha256_once(const uint8_t *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    if (!sha256_starts(&ctx)) return false;
    if (!sha256_update(&ctx, data, len)) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    return sha256_finish(&ctx, out);
}

static bool file_write_at(const char *path, uint32_t offset, const uint8_t *data, size_t len)
{
    storage_file_t f = {0};
    if (!storage_open_write(&f, path, false)) return false;
    bool ok = storage_seek(&f, offset);
    size_t put = 0;
    if (ok && len) ok = storage_write(&f, data, len, &put) && put == len;
    if (ok) ok = storage_sync(&f);
    storage_close(&f);
    return ok;
}

static bool key_id_for(const remote_auth_key_t *key, uint8_t key_id[16])
{
    uint8_t hash[32];
    if (!key || !sha256_once(key->bytes, key->len, hash)) return false;
    memcpy(key_id, hash, 16);
    return true;
}

static bool verify_p256_signature(const remote_auth_key_t *key,
                                  const uint8_t digest[32],
                                  const uint8_t sig[64])
{
    bool ok = false;
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) goto out;
    if (mbedtls_ecp_point_read_binary(&grp, &q, key->bytes, key->len) != 0) goto out;
    if (mbedtls_mpi_read_binary(&r, sig, 32) != 0) goto out;
    if (mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0) goto out;
    ok = mbedtls_ecdsa_verify(&grp, digest, 32, &q, &r, &s) == 0;

out:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

static bool verify_block(const remote_auth_config_t *cfg,
                         m65_signed_file_type_t expected_type,
                         const char *expected_path,
                         uint32_t payload_len,
                         const uint8_t payload_hash[32],
                         const uint8_t block[M65_SIG_BLOCK_SIZE])
{
    if (!cfg || cfg->trusted_key_count == 0) {
        set_err("no trusted signature keys configured");
        return false;
    }
    if (memcmp(block + SIG_OFF_MAGIC, sig_magic, sizeof sig_magic) != 0) {
        set_err("signature block magic missing");
        return false;
    }
    if (le16(block + SIG_OFF_VERSION) != SIG_VERSION ||
        le16(block + SIG_OFF_HEADER_LEN) != M65_SIG_BLOCK_SIZE) {
        set_err("unsupported signature block version");
        return false;
    }
    if (le32(block + SIG_OFF_PAYLOAD_LEN) != payload_len) {
        set_err("signature payload length mismatch");
        return false;
    }
    uint8_t type = block[SIG_OFF_FILE_TYPE];
    if (type != M65_SIGNED_FILE_ANY &&
        expected_type != M65_SIGNED_FILE_ANY &&
        type != (uint8_t)expected_type) {
        set_err("signature file type mismatch");
        return false;
    }
    if (block[SIG_OFF_HASH_ALG] != SIG_HASH_SHA256 ||
        block[SIG_OFF_SIG_ALG] != SIG_ALG_ECDSA_P256_SHA256) {
        set_err("unsupported signature algorithm");
        return false;
    }
    if (memcmp(block + SIG_OFF_PAYLOAD_SHA256, payload_hash, 32) != 0) {
        set_err("payload SHA-256 mismatch");
        return false;
    }

    const char *expected_name = expected_path ? strrchr(expected_path, '/') : NULL;
    expected_name = expected_name ? expected_name + 1 : expected_path;
    const char *signed_name = (const char *)(block + SIG_OFF_FILENAME);
    if (signed_name[0]) {
        size_t signed_len = bounded_cstr_len(signed_name, SIG_FILENAME_LEN);
        if (signed_len >= SIG_FILENAME_LEN) {
            set_err("signature filename is not terminated");
            return false;
        }
        if (!expected_name || strcmp(signed_name, expected_name) != 0) {
            set_err("signature filename mismatch");
            return false;
        }
    }

    uint8_t signed_digest[32];
    if (!sha256_once(block, SIG_OFF_SIGNATURE, signed_digest)) {
        set_err("cannot hash signature metadata");
        return false;
    }

    const uint8_t *want_key_id = block + SIG_OFF_KEY_ID;
    for (size_t i = 0; i < cfg->trusted_key_count; ++i) {
        const remote_auth_key_t *key = &cfg->trusted_keys[i];
        if (key->alg != REMOTE_AUTH_KEY_P256) continue;
        if (!is_zero16(want_key_id)) {
            uint8_t have_key_id[16];
            if (!key_id_for(key, have_key_id)) continue;
            if (memcmp(have_key_id, want_key_id, 16) != 0) continue;
        }
        if (verify_p256_signature(key, signed_digest, block + SIG_OFF_SIGNATURE)) {
            last_err[0] = 0;
            return true;
        }
    }

    set_err("signature verification failed");
    return false;
}

const char *signed_file_type_name(m65_signed_file_type_t type)
{
    switch (type) {
    case M65_SIGNED_FILE_BIT: return "bit";
    case M65_SIGNED_FILE_COR: return "cor";
    case M65_SIGNED_FILE_M65J: return "m65j";
    default: return "any";
    }
}

static bool ext_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

m65_signed_file_type_t signed_file_type_from_path(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return M65_SIGNED_FILE_ANY;
    if (ext_equal_ci(dot, ".bit")) return M65_SIGNED_FILE_BIT;
    if (ext_equal_ci(dot, ".cor")) return M65_SIGNED_FILE_COR;
    if (ext_equal_ci(dot, ".m65j")) return M65_SIGNED_FILE_M65J;
    return M65_SIGNED_FILE_ANY;
}

bool signed_file_receive_begin(signed_file_rx_t *rx,
                               const remote_auth_config_t *cfg,
                               const char *final_path,
                               const char *tmp_path,
                               uint32_t total_length,
                               m65_signed_file_type_t file_type)
{
    if (!rx || !cfg || !final_path || !tmp_path || total_length == 0) {
        set_err("invalid signed receive parameters");
        return false;
    }
    memset(rx, 0, sizeof *rx);
    snprintf(rx->final_path, sizeof rx->final_path, "%s", final_path);
    snprintf(rx->tmp_path, sizeof rx->tmp_path, "%s", tmp_path);
    rx->cfg = cfg;
    rx->total_length = total_length;
    rx->require_signature = cfg->require_signatures;
    rx->file_type = file_type;

    bool candidate = total_length > M65_SIG_BLOCK_SIZE;
    rx->candidate_signature = candidate;
    rx->block_offset = candidate ? total_length - M65_SIG_BLOCK_SIZE : total_length;

    if (rx->require_signature && !candidate) {
        set_err("signed file must end with a 256-byte signature block");
        return false;
    }
    if (rx->require_signature && cfg->trusted_key_count == 0) {
        set_err("require_signatures is set but no trusted_key entries are configured");
        return false;
    }

    if (candidate && !sha256_starts(&rx->sha)) {
        set_err("cannot initialise SHA-256");
        return false;
    }

    last_err[0] = 0;
    return true;
}

bool signed_file_receive_write(signed_file_rx_t *rx, const uint8_t *data, size_t len)
{
    if (!rx || (!data && len) || rx->rejected) return false;
    if (rx->done + len > rx->total_length) {
        rx->rejected = true;
        set_err("receive exceeded declared length");
        return false;
    }

    while (len) {
        if (rx->candidate_signature && rx->done >= rx->block_offset) {
            size_t want = M65_SIG_BLOCK_SIZE - rx->block_done;
            if (want > len) want = len;
            memcpy(rx->block + rx->block_done, data, want);
            rx->block_done += (uint32_t)want;
            rx->done += (uint32_t)want;
            data += want;
            len -= want;
            continue;
        }

        uint32_t payload_limit = rx->candidate_signature ? rx->block_offset : rx->total_length;
        size_t want = payload_limit - rx->done;
        if (want > len) want = len;
        if (want) {
            if (!file_write_at(rx->tmp_path, rx->payload_done, data, want)) {
                rx->rejected = true;
                snprintf(last_err, sizeof last_err, "write failed: %s", storage_last_error());
                return false;
            }
            if (rx->candidate_signature && !sha256_update(&rx->sha, data, want)) {
                rx->rejected = true;
                set_err("SHA-256 update failed");
                return false;
            }
            rx->payload_done += (uint32_t)want;
            rx->done += (uint32_t)want;
            data += want;
            len -= want;
        }
    }
    return true;
}

bool signed_file_receive_finish(signed_file_rx_t *rx)
{
    if (!rx || rx->rejected) return false;
    if (rx->done != rx->total_length) {
        set_err("short signed receive");
        return false;
    }

    if (!rx->candidate_signature) {
        last_err[0] = 0;
        return true;
    }

    rx->saw_magic = rx->block_done == M65_SIG_BLOCK_SIZE &&
                    memcmp(rx->block, sig_magic, sizeof sig_magic) == 0;

    if (!rx->saw_magic && !rx->require_signature) {
        if (!file_write_at(rx->tmp_path, rx->payload_done, rx->block, rx->block_done)) {
            snprintf(last_err, sizeof last_err, "write failed: %s", storage_last_error());
            mbedtls_sha256_free(&rx->sha);
            return false;
        }
        mbedtls_sha256_free(&rx->sha);
        last_err[0] = 0;
        return true;
    }

    uint8_t payload_hash[32];
    if (!sha256_finish(&rx->sha, payload_hash)) {
        set_err("SHA-256 finish failed");
        return false;
    }
    if (!verify_block(rx->cfg, rx->file_type, rx->final_path, rx->block_offset, payload_hash, rx->block)) {
        return false;
    }

    last_err[0] = 0;
    return true;
}

void signed_file_receive_abort(signed_file_rx_t *rx)
{
    if (!rx) return;
    if (rx->candidate_signature) mbedtls_sha256_free(&rx->sha);
    if (rx->tmp_path[0]) storage_delete(rx->tmp_path);
    rx->rejected = true;
}
