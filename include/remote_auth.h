#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMOTE_AUTH_MAX_RULES 8
#define REMOTE_AUTH_MAX_KEYS 4
#define REMOTE_AUTH_MAX_KEY_BYTES 80

typedef enum {
    REMOTE_AUTH_FILES = 0,
    REMOTE_AUTH_BITSTREAMS,
} remote_auth_perm_t;

typedef enum {
    REMOTE_AUTH_KEY_NONE = 0,
    REMOTE_AUTH_KEY_P256,
} remote_auth_key_alg_t;

typedef struct {
    uint32_t network;
    uint32_t mask;
    bool allow_files;
    bool allow_bitstreams;
} remote_auth_rule_t;

typedef struct {
    remote_auth_key_alg_t alg;
    uint8_t bytes[REMOTE_AUTH_MAX_KEY_BYTES];
    size_t len;
} remote_auth_key_t;

typedef struct {
    bool present;
    bool dhcp;
    bool http_enabled;
    bool autofetch_enabled;
    bool require_write_grant;
    bool require_signatures;
    uint16_t http_port;
    uint32_t fetch_interval_hours;
    uint8_t fetch_board_rev;
    char wifi_ssid[33];
    char wifi_psk[65];
    char http_user[33];
    char http_password[65];
    char fetch_base_url[192];
    char fetch_channel[24];
    uint32_t static_ip;
    uint32_t netmask;
    uint32_t gateway;
    remote_auth_rule_t rules[REMOTE_AUTH_MAX_RULES];
    size_t rule_count;
    remote_auth_key_t trusted_keys[REMOTE_AUTH_MAX_KEYS];
    size_t trusted_key_count;
} remote_auth_config_t;

void remote_auth_reset(remote_auth_config_t *cfg);
bool remote_auth_load(remote_auth_config_t *cfg, char *err, size_t err_len);
bool remote_auth_allowed(const remote_auth_config_t *cfg, uint32_t remote_ip, remote_auth_perm_t perm);
bool remote_auth_parse_ipv4(const char *s, uint32_t *ip_out);
void remote_auth_format_ipv4(uint32_t ip, char *buf, size_t buflen);
