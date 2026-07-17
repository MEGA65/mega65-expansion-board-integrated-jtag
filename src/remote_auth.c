#include "remote_auth.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REMOTE_CONFIG_PATH "mega65-jtag.cfg"
#define REMOTE_CONFIG_LEGACY_PATH "REMOTE_ENABLE.cfg"
#define REMOTE_CFG_MAX_BYTES 4096u

static char cfg_buf[REMOTE_CFG_MAX_BYTES + 1u];

void remote_auth_reset(remote_auth_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->dhcp = true;
    cfg->http_enabled = true;
    cfg->require_write_grant = true;
    cfg->http_port = 80;
    cfg->fetch_interval_hours = 3;
    snprintf(cfg->fetch_channel, sizeof cfg->fetch_channel, "stable");
    cfg->netmask = 0xffffff00u;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool parse_bool_value(const char *s, bool *out)
{
    if (ci_equal(s, "1") || ci_equal(s, "yes") || ci_equal(s, "true") || ci_equal(s, "on")) {
        *out = true;
        return true;
    }
    if (ci_equal(s, "0") || ci_equal(s, "no") || ci_equal(s, "false") || ci_equal(s, "off")) {
        *out = false;
        return true;
    }
    return false;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_board_value(const char *s, uint8_t *out)
{
    if (!s || !out) return false;
    if (ci_equal(s, "3") || ci_equal(s, "r3")) {
        *out = 3;
        return true;
    }
    if (ci_equal(s, "6") || ci_equal(s, "r6")) {
        *out = 6;
        return true;
    }
    return false;
}

static bool parse_hex_bytes(const char *s, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t n = 0;
    int high = -1;
    while (*s) {
        if (*s == ':' || *s == '-' || isspace((unsigned char)*s)) {
            s++;
            continue;
        }
        int v = hex_value(*s++);
        if (v < 0) return false;
        if (high < 0) {
            high = v;
        } else {
            if (n >= out_cap) return false;
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0) return false;
    if (out_len) *out_len = n;
    return n != 0;
}

static bool parse_trusted_key(remote_auth_config_t *cfg, const char *value)
{
    if (cfg->trusted_key_count >= REMOTE_AUTH_MAX_KEYS) return false;

    remote_auth_key_alg_t alg = REMOTE_AUTH_KEY_P256;
    const char *hex = value;
    if (ci_starts_with(value, "p256:")) {
        hex = value + 5;
    } else if (ci_starts_with(value, "ecdsa-p256:")) {
        hex = value + 11;
    }

    remote_auth_key_t key = { .alg = alg };
    if (!parse_hex_bytes(hex, key.bytes, sizeof key.bytes, &key.len)) return false;

    // Only raw uncompressed P-256 public keys are accepted in v1:
    // 04 || X(32) || Y(32)
    if (key.alg == REMOTE_AUTH_KEY_P256 && (key.len != 65u || key.bytes[0] != 0x04u)) return false;

    cfg->trusted_keys[cfg->trusted_key_count++] = key;
    return true;
}

bool remote_auth_parse_ipv4(const char *s, uint32_t *ip_out)
{
    uint32_t parts[4] = {0};
    const char *p = s;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isdigit((unsigned char)*p)) return false;
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (v > 255u) return false;
        parts[i] = (uint32_t)v;
        p = end;
        if (i < 3) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p && !isspace((unsigned char)*p) && *p != '/' && *p != ',' && *p != ';') return false;
    if (ip_out) {
        *ip_out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    }
    return true;
}

void remote_auth_format_ipv4(uint32_t ip, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xffu),
             (unsigned long)((ip >> 16) & 0xffu),
             (unsigned long)((ip >> 8) & 0xffu),
             (unsigned long)(ip & 0xffu));
}

static bool parse_cidr(char *token, uint32_t *network_out, uint32_t *mask_out)
{
    char *slash = strchr(token, '/');
    unsigned prefix = 32;
    if (slash) {
        *slash++ = 0;
        char *end = NULL;
        unsigned long v = strtoul(slash, &end, 10);
        if (*end || v > 32u) return false;
        prefix = (unsigned)v;
    }

    uint32_t ip = 0;
    if (!remote_auth_parse_ipv4(token, &ip)) return false;
    uint32_t mask = prefix == 0 ? 0u : (0xffffffffu << (32u - prefix));
    *network_out = ip & mask;
    *mask_out = mask;
    return true;
}

static bool split_key_value(char *token, char **key_out, char **value_out)
{
    char *eq = strchr(token, '=');
    if (!eq) return false;
    *eq++ = 0;
    *key_out = trim(token);
    *value_out = trim(eq);
    return **key_out && **value_out;
}

static bool parse_rule_option(remote_auth_rule_t *rule, char *token)
{
    char *key = NULL;
    char *value = NULL;
    if (!split_key_value(token, &key, &value)) return false;

    bool b = false;
    if (ci_equal(key, "files") || ci_equal(key, "file") || ci_equal(key, "sd")) {
        if (!parse_bool_value(value, &b)) return false;
        rule->allow_files = b;
        return true;
    }
    if (ci_equal(key, "bitstreams") || ci_equal(key, "bitstream") ||
        ci_equal(key, "jtag") || ci_equal(key, "program")) {
        if (!parse_bool_value(value, &b)) return false;
        rule->allow_bitstreams = b;
        return true;
    }
    return false;
}

static bool parse_rule(remote_auth_config_t *cfg, char *line)
{
    if (cfg->rule_count >= REMOTE_AUTH_MAX_RULES) return false;

    char *save = NULL;
    char *first = strtok_r(line, " \t,", &save);
    if (!first) return false;

    if (strchr(first, '=')) {
        char *key = NULL;
        char *value = NULL;
        if (!split_key_value(first, &key, &value)) return false;
        if (!ci_equal(key, "range") && !ci_equal(key, "allow")) return false;
        first = value;
    }

    remote_auth_rule_t rule = {0};
    if (!parse_cidr(first, &rule.network, &rule.mask)) return false;

    char *tok = NULL;
    while ((tok = strtok_r(NULL, " \t,", &save)) != NULL) {
        if (!parse_rule_option(&rule, tok)) return false;
    }

    cfg->rules[cfg->rule_count++] = rule;
    return true;
}

static bool parse_global(remote_auth_config_t *cfg, char *line)
{
    char *key = NULL;
    char *value = NULL;
    if (!split_key_value(line, &key, &value)) return false;

    if (ci_equal(key, "dhcp")) {
        return parse_bool_value(value, &cfg->dhcp);
    }
    if (ci_equal(key, "http") || ci_equal(key, "http_enabled") || ci_equal(key, "http_enable")) {
        return parse_bool_value(value, &cfg->http_enabled);
    }
    if (ci_equal(key, "autofetch") || ci_equal(key, "auto_fetch") || ci_equal(key, "auto_update")) {
        return parse_bool_value(value, &cfg->autofetch_enabled);
    }
    if (ci_equal(key, "fetch_interval") ||
        ci_equal(key, "fetch_interval_hours") ||
        ci_equal(key, "autofetch_interval")) {
        char *end = NULL;
        unsigned long hours = strtoul(value, &end, 10);
        if (*end || hours < 3u || hours > 8760u) return false;
        cfg->fetch_interval_hours = (uint32_t)hours;
        return true;
    }
    if (ci_equal(key, "fetch_board") ||
        ci_equal(key, "fetch_board_rev") ||
        ci_equal(key, "autofetch_board") ||
        ci_equal(key, "board_rev")) {
        return parse_board_value(value, &cfg->fetch_board_rev);
    }
    if (ci_equal(key, "fetch_base_url") ||
        ci_equal(key, "mirror_url") ||
        ci_equal(key, "autofetch_url") ||
        ci_equal(key, "update_url")) {
        snprintf(cfg->fetch_base_url, sizeof cfg->fetch_base_url, "%s", value);
        return true;
    }
    if (ci_equal(key, "fetch_channel") ||
        ci_equal(key, "mirror_channel") ||
        ci_equal(key, "update_channel") ||
        ci_equal(key, "channel")) {
        snprintf(cfg->fetch_channel, sizeof cfg->fetch_channel, "%s", value);
        return true;
    }
    if (ci_equal(key, "http_port") || ci_equal(key, "port")) {
        char *end = NULL;
        unsigned long port = strtoul(value, &end, 10);
        if (*end || port == 0 || port > 65535u) return false;
        cfg->http_port = (uint16_t)port;
        return true;
    }
    if (ci_equal(key, "require_write_grant") ||
        ci_equal(key, "write_grant_required") ||
        ci_equal(key, "write_grant")) {
        return parse_bool_value(value, &cfg->require_write_grant);
    }
    if (ci_equal(key, "require_signatures") ||
        ci_equal(key, "signed_files_required") ||
        ci_equal(key, "signature_required")) {
        return parse_bool_value(value, &cfg->require_signatures);
    }
    if (ci_equal(key, "mode") || ci_equal(key, "ip_mode")) {
        if (ci_equal(value, "dhcp")) {
            cfg->dhcp = true;
            return true;
        }
        if (ci_equal(value, "static")) {
            cfg->dhcp = false;
            return true;
        }
        return false;
    }
    if (ci_equal(key, "ip") || ci_equal(key, "addr") || ci_equal(key, "address")) {
        cfg->dhcp = false;
        return remote_auth_parse_ipv4(value, &cfg->static_ip);
    }
    if (ci_equal(key, "netmask") || ci_equal(key, "mask")) {
        cfg->dhcp = false;
        return remote_auth_parse_ipv4(value, &cfg->netmask);
    }
    if (ci_equal(key, "gateway") || ci_equal(key, "gw")) {
        cfg->dhcp = false;
        return remote_auth_parse_ipv4(value, &cfg->gateway);
    }
    if (ci_equal(key, "ssid") || ci_equal(key, "wifi_ssid")) {
        snprintf(cfg->wifi_ssid, sizeof cfg->wifi_ssid, "%s", value);
        return true;
    }
    if (ci_equal(key, "http_user") || ci_equal(key, "http_username") || ci_equal(key, "user")) {
        snprintf(cfg->http_user, sizeof cfg->http_user, "%s", value);
        return true;
    }
    if (ci_equal(key, "http_password") || ci_equal(key, "http_pass")) {
        snprintf(cfg->http_password, sizeof cfg->http_password, "%s", value);
        return true;
    }
    if (ci_equal(key, "psk") || ci_equal(key, "password") || ci_equal(key, "wifi_psk")) {
        snprintf(cfg->wifi_psk, sizeof cfg->wifi_psk, "%s", value);
        return true;
    }
    if (ci_equal(key, "trusted_key") ||
        ci_equal(key, "public_key") ||
        ci_equal(key, "signature_key")) {
        return parse_trusted_key(cfg, value);
    }
    return false;
}

bool remote_auth_load(remote_auth_config_t *cfg, char *err, size_t err_len)
{
    remote_auth_reset(cfg);
    if (err && err_len) err[0] = 0;

    storage_file_t f = {0};
    const char *path = REMOTE_CONFIG_PATH;
    if (!storage_open(&f, path)) {
        path = REMOTE_CONFIG_LEGACY_PATH;
        if (!storage_open(&f, path)) {
            if (err && err_len) {
                snprintf(err, err_len, "%s not found", REMOTE_CONFIG_PATH);
            }
            return false;
        }
    }

    uint32_t size = storage_size(&f);
    if (size > REMOTE_CFG_MAX_BYTES) {
        storage_close(&f);
        if (err && err_len) snprintf(err, err_len, "%s too large", path);
        return false;
    }

    size_t got = 0;
    bool ok = storage_read(&f, cfg_buf, size, &got);
    storage_close(&f);
    if (!ok || got != size) {
        if (err && err_len) snprintf(err, err_len, "read failed");
        return false;
    }
    cfg_buf[size] = 0;

    cfg->present = true;
    unsigned line_no = 0;
    char *save = NULL;
    char *line = strtok_r(cfg_buf, "\n", &save);
    while (line) {
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        line = trim(line);
        if (*line) {
            bool is_rule = isdigit((unsigned char)line[0]) ||
                           ci_starts_with(line, "range=") ||
                           ci_starts_with(line, "allow=");
            bool parsed = is_rule ? parse_rule(cfg, line) : parse_global(cfg, line);
            if (!parsed) {
                if (err && err_len) snprintf(err, err_len, "parse error line %u", line_no);
                remote_auth_reset(cfg);
                return false;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }

    if (cfg->rule_count == 0) {
        if (err && err_len) snprintf(err, err_len, "no allow rules");
        remote_auth_reset(cfg);
        return false;
    }

    return true;
}

bool remote_auth_allowed(const remote_auth_config_t *cfg, uint32_t remote_ip, remote_auth_perm_t perm)
{
    if (!cfg || !cfg->present) return false;
    for (size_t i = 0; i < cfg->rule_count; ++i) {
        const remote_auth_rule_t *r = &cfg->rules[i];
        if ((remote_ip & r->mask) != r->network) continue;
        if (perm == REMOTE_AUTH_FILES) return r->allow_files;
        if (perm == REMOTE_AUTH_BITSTREAMS) return r->allow_bitstreams;
    }
    return false;
}
