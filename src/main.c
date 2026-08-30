#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "config.h"
#include "uart_cmd.h"
#include "storage.h"
#include "bootsel_button.h"
#include "core_file.h"
#include "core_filter.h"
#include "jtag_gpio.h"
#include "machine_identity.h"
#include "remote_auth.h"
#include "remote_http.h"
#include "write_gate.h"

#ifndef M65_WRITE_ENABLE_PIN
#define M65_WRITE_ENABLE_PIN 255u
#endif
#ifndef M65_WRITE_ENABLE_ACTIVE_LOW
#define M65_WRITE_ENABLE_ACTIVE_LOW 1
#endif
#ifndef M65_WRITE_ENABLE_TIMEOUT_MS
#define M65_WRITE_ENABLE_TIMEOUT_MS 120000u
#endif
#ifndef M65_WIFI_COMMAND_QUIET_MS
#define M65_WIFI_COMMAND_QUIET_MS 15000u
#endif
#ifndef M65_SD_HOTPLUG_POLL_MS
#define M65_SD_HOTPLUG_POLL_MS 2000u
#endif
#ifndef M65_WRITE_COMMANDS_USB_ONLY
#define M65_WRITE_COMMANDS_USB_ONLY 1
#endif
#ifndef M65_STREAM_COMMANDS_USB_ONLY
#define M65_STREAM_COMMANDS_USB_ONLY 1
#endif

#define AT_SETTINGS_PATH "AT_SETTINGS.cfg"
#ifndef M65_CORE_LIST_INDEX_MAX
#define M65_CORE_LIST_INDEX_MAX 128u
#endif
#ifndef M65_CORE_LIST_INDEX_PATH_POOL
#define M65_CORE_LIST_INDEX_PATH_POOL 8192u
#endif

static bool sd_mounted = false;
static remote_auth_config_t remote_cfg;

typedef enum {
    CMD_MODE_AT = 0,
    CMD_MODE_BASIC,
} command_mode_t;

static command_mode_t command_mode = CMD_MODE_AT;
static bool usb_reconnect_pending = false;
static absolute_time_t usb_reconnect_at;
static bool remote_init_done = false;
static bool sd_config_loaded = false;
static absolute_time_t sd_hotplug_poll_at;
static bool is_partial_core_path(const char *path);
static bool make_child_path(const char *dir, const char *name, char *out, size_t out_len);

typedef struct {
    uint16_t number;
    uint16_t path_off;
    uint16_t path_len;
    bool is_dir;
    bool is_partial;
} core_list_index_entry_t;

static core_list_index_entry_t core_list_index[M65_CORE_LIST_INDEX_MAX];
static char core_list_path_pool[M65_CORE_LIST_INDEX_PATH_POOL];
static uint16_t core_list_index_count;
static uint16_t core_list_path_pool_used;
static bool core_list_index_valid;

typedef struct {
    const char *dir;
    uint16_t next_number;
} numbered_list_ctx_t;

typedef struct {
    int autofetch; // -1 = use mega65-jtag.cfg default, 0 = off, 1 = on
    uint32_t fetch_interval_hours;
    uint8_t fetch_board_rev; // 0 = use mega65-jtag.cfg default
    uint8_t verbose; // 0 = quiet, 1 = normal diagnostics, 2 = progress
} at_settings_t;

static at_settings_t at_settings = {
    .autofetch = -1,
    .fetch_interval_hours = 0,
    .fetch_board_rev = 0,
    .verbose = 1,
};

static void progress_cb(uint32_t done, uint32_t total, void *ctx)
{
    (void)ctx;
    uart_cmd_printf("PROG %lu/%lu\n", (unsigned long)done, (unsigned long)total);
}

static void usb_reconnect_schedule(void)
{
#if M65_ENABLE_USB_CDC
    usb_reconnect_pending = true;
    usb_reconnect_at = make_timeout_time_ms(250);
#endif
}

static void usb_reconnect_poll(void)
{
#if M65_ENABLE_USB_CDC
    if (!usb_reconnect_pending ||
        absolute_time_diff_us(get_absolute_time(), usb_reconnect_at) > 0) {
        return;
    }
    usb_reconnect_pending = false;
    tud_disconnect();
    sleep_ms(250);
    tud_connect();
#endif
}

static bool ensure_mount(void)
{
    if (sd_mounted || storage_is_mounted()) {
        sd_mounted = true;
        return true;
    }
    if (!storage_mount()) {
        uart_cmd_printf("ERR SD %s\n", storage_last_error());
        return false;
    }
    sd_mounted = true;
    return true;
}

static bool ensure_mount_quiet(void)
{
    if (sd_mounted || storage_is_mounted()) {
        sd_mounted = true;
        return true;
    }
    if (!storage_mount()) return false;
    sd_mounted = true;
    return true;
}

static void uart_quoted(const char *s)
{
    uart_cmd_puts("\"");
    if (s) {
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') uart_cmd_printf("\\%c", *s);
            else if ((unsigned char)*s >= 32u && (unsigned char)*s < 127u) uart_cmd_printf("%c", *s);
            else uart_cmd_printf("\\x%02x", (unsigned)((unsigned char)*s));
        }
    }
    uart_cmd_puts("\"");
}

static bool make_child_path(const char *dir, const char *name, char *out, size_t out_len)
{
    if (!dir || !dir[0] || strcmp(dir, "/") == 0) {
        return snprintf(out, out_len, "/%s", name) < (int)out_len;
    }
    size_t n = strlen(dir);
    const char *sep = (n && dir[n - 1] == '/') ? "" : "/";
    return snprintf(out, out_len, "%s%s%s", dir, sep, name) < (int)out_len;
}

static void core_list_index_begin(void)
{
    core_list_index_count = 0;
    core_list_path_pool_used = 0;
    core_list_index_valid = false;
}

static void core_list_index_commit(void)
{
    core_list_index_valid = true;
}

static const char *core_list_index_path(const core_list_index_entry_t *entry)
{
    if (!entry || entry->path_off >= core_list_path_pool_used) return "";
    return core_list_path_pool + entry->path_off;
}

static bool core_list_index_add(uint16_t number, const char *dir, const char *name, bool is_dir, bool is_partial)
{
    if (core_list_index_count >= M65_CORE_LIST_INDEX_MAX) return false;

    char path[256];
    if (!make_child_path(dir, name, path, sizeof path)) return false;

    size_t path_len = strlen(path);
    if (path_len + 1u > sizeof core_list_path_pool - core_list_path_pool_used) return false;

    core_list_index_entry_t *entry = &core_list_index[core_list_index_count++];
    entry->number = number;
    entry->path_off = core_list_path_pool_used;
    entry->path_len = (uint16_t)path_len;
    entry->is_dir = is_dir;
    entry->is_partial = is_partial;
    memcpy(core_list_path_pool + entry->path_off, path, path_len + 1u);
    core_list_path_pool_used = (uint16_t)(core_list_path_pool_used + path_len + 1u);
    return true;
}

static const core_list_index_entry_t *core_list_index_find(uint16_t number)
{
    if (!core_list_index_valid || number == 0) return NULL;
    for (uint16_t i = 0; i < core_list_index_count; i++) {
        if (core_list_index[i].number == number) return &core_list_index[i];
    }
    return NULL;
}

static void list_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    numbered_list_ctx_t *lc = (numbered_list_ctx_t *)ctx;
    uint16_t number = lc ? lc->next_number++ : 0;
    char path[256];
    bool have_path = make_child_path(lc ? lc->dir : "/", name, path, sizeof path);
    bool is_partial = is_partial_core_path(have_path ? path : name);

    if (number) {
        (void)core_list_index_add(number, lc ? lc->dir : "/", name, is_dir, is_partial);
    }

    uart_cmd_printf("%3lu %s %lu %s\n",
                    (unsigned long)number,
                    is_dir ? "DIR" : (is_partial ? "PARTIAL" : "CORE"),
                    (unsigned long)size,
                    name);
}

static uint8_t core_effective_board(const core_file_t *cf, const char *path)
{
    if (cf && cf->model_id) return cf->model_id;
    return core_board_hint_from_name(path);
}

static void print_xilinx_status(const char *prefix, const jtag_status_t *st)
{
    if (!st) {
        uart_cmd_printf("%s invalid\n", prefix);
        return;
    }

    uint32_t stat = st->stat;
    uart_cmd_printf(
        "%s idcode=%08lx bootsts=%s%08lx stat=%s%08lx bypass=%s%08lx stat_done=%lu release_done=%lu eos=%lu startup=%lu\n",
        prefix,
        (unsigned long)st->idcode,
        st->bootsts_valid ? "" : "?", (unsigned long)st->bootsts,
        st->stat_valid ? "" : "?", (unsigned long)st->stat,
        st->bypass_valid ? "" : "?", (unsigned long)st->bypass,
        (unsigned long)((stat >> 14) & 1u),
        (unsigned long)((stat >> 13) & 1u),
        (unsigned long)((stat >> 4) & 1u),
        (unsigned long)((stat >> 18) & 7u));
}

static void activity_led_init(void)
{
#if M65_LED_PIN != 255
    gpio_init(M65_LED_PIN);
    gpio_set_dir(M65_LED_PIN, GPIO_OUT);
    gpio_put(M65_LED_PIN, 0);
#endif
}

static void activity_led_put(bool on)
{
#if M65_LED_PIN != 255
    gpio_put(M65_LED_PIN, on ? 1 : 0);
#else
    (void)on;
#endif
}

static void boot_diag_pin_init(void)
{
#if M65_BOOT_DIAG && M65_BOOT_DIAG_PIN != 255
    gpio_init(M65_BOOT_DIAG_PIN);
    gpio_set_dir(M65_BOOT_DIAG_PIN, GPIO_OUT);
    gpio_put(M65_BOOT_DIAG_PIN, 0);
#endif
}

static void boot_diag_uart_init(void)
{
#if M65_BOOT_DIAG
    uart_init(M65_UART_ID, M65_BOOT_DIAG_UART_BAUD);
    gpio_set_function(M65_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(M65_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(M65_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(M65_UART_ID, false, false);
    uart_set_fifo_enabled(M65_UART_ID, true);
#endif
}

static void boot_diag_stage(unsigned stage, const char *label)
{
#if M65_BOOT_DIAG
    uart_puts(M65_UART_ID, "M65APP ");
    uart_putc_raw(M65_UART_ID, (char)('0' + (stage % 10u)));
    uart_puts(M65_UART_ID, " ");
    uart_puts(M65_UART_ID, label);
    uart_puts(M65_UART_ID, "\r\n");
    uart_tx_wait_blocking(M65_UART_ID);
#if M65_BOOT_DIAG_PIN != 255
    for (unsigned i = 0; i < stage; i++) {
        gpio_put(M65_BOOT_DIAG_PIN, 1);
        sleep_ms(70);
        gpio_put(M65_BOOT_DIAG_PIN, 0);
        sleep_ms(90);
    }
    sleep_ms(180);
#endif
#else
    (void)stage;
    (void)label;
#endif
}

static bool write_command_source_allowed(void)
{
#if M65_WRITE_COMMANDS_USB_ONLY
    return uart_cmd_last_source() == UART_CMD_SRC_USB;
#else
    return true;
#endif
}

static bool stream_command_source_allowed(void)
{
#if M65_STREAM_COMMANDS_USB_ONLY
    return uart_cmd_last_source() == UART_CMD_SRC_USB;
#else
    return true;
#endif
}

static bool require_write_authority(const char *cmd)
{
    if (!write_command_source_allowed()) {
        uart_cmd_printf("ERR %s write commands require USB source; source=%s\n",
                        cmd, uart_cmd_source_name(uart_cmd_last_source()));
        return false;
    }
#if M65_WRITE_ENABLE_PIN == 255
    uart_cmd_printf("ERR %s write commands disabled: M65_WRITE_ENABLE_PIN=255\n", cmd);
    return false;
#else
    if (!write_gate_active()) {
        uart_cmd_printf("ERR %s write protected: assert WE pin GP%u to grant %lu ms; source=%s physical=%lu\n",
                        cmd,
                        (unsigned)M65_WRITE_ENABLE_PIN,
                        (unsigned long)M65_WRITE_ENABLE_TIMEOUT_MS,
                        uart_cmd_source_name(uart_cmd_last_source()),
                        (unsigned long)(write_gate_physical_asserted() ? 1u : 0u));
        return false;
    }
    return true;
#endif
}

static bool has_core_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char e1 = (char)tolower((unsigned char)dot[1]);
    char e2 = (char)tolower((unsigned char)dot[2]);
    char e3 = (char)tolower((unsigned char)dot[3]);
    char e4 = (char)tolower((unsigned char)dot[4]);
    char e5 = (char)tolower((unsigned char)dot[5]);
    return (e1 == 'b' && e2 == 'i' && e3 == 't' && e4 == 0) ||
           (e1 == 'c' && e2 == 'o' && e3 == 'r' && e4 == 0) ||
           (e1 == 'm' && e2 == '6' && e3 == '5' && e4 == 'j' && e5 == 0);
}

static bool has_suffix_ci(const char *name, const char *suffix)
{
    size_t n = strlen(name ? name : "");
    size_t sn = strlen(suffix ? suffix : "");
    if (n < sn) return false;
    const char *tail = name + n - sn;
    for (size_t i = 0; i < sn; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static bool is_partial_core_path(const char *path)
{
    static const char suffix[] = ".partial";
    if (!has_suffix_ci(path, suffix)) return false;
    size_t n = strlen(path);
    size_t base_len = n - (sizeof suffix - 1u);
    if (base_len == 0 || base_len >= 256u) return false;
    char base[256];
    memcpy(base, path, base_len);
    base[base_len] = 0;
    return has_core_ext(base);
}

static bool safe_core_write_path(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 220) return false; // leaves room for .partial and FatFs path buffer
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    return has_core_ext(path);
}

static bool safe_download_name(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 180) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    return true;
}

static bool make_download_path(const char *name, char *out, size_t out_len)
{
    if (!safe_download_name(name)) return false;
    return snprintf(out, out_len, "DOWNLOADS/%s", name) < (int)out_len;
}

static bool ci_starts_with(const char *s, const char *prefix);

static bool parse_download_slot_token(const char *token, int *slot)
{
    if (slot) *slot = -1;
    if (!token || !token[0]) return true;

    char buf[32];
    size_t n = strlen(token);
    if (n >= sizeof buf) return false;
    snprintf(buf, sizeof buf, "%s", token);

    char *s = trim_line(buf);
    if (ci_starts_with(s, "download-")) s += 9;
    char *dot = strchr(s, '.');
    if (dot) *dot = 0;
    if (ci_starts_with(s, "0x")) s += 2;
    if (!s[0] || strlen(s) > 2) return false;

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (!end || *end || v > 255u) return false;
    if (slot) *slot = (int)v;
    return true;
}

static bool ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool parse_bool_value(const char *s, bool *out)
{
    if (ci_equal(s, "1") || ci_equal(s, "ON") || ci_equal(s, "YES") || ci_equal(s, "TRUE")) {
        *out = true;
        return true;
    }
    if (ci_equal(s, "0") || ci_equal(s, "OFF") || ci_equal(s, "NO") || ci_equal(s, "FALSE")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_board_value(const char *s, uint8_t *out)
{
    if (!s || !out) return false;
    if (ci_equal(s, "3") || ci_equal(s, "R3")) {
        *out = 3;
        return true;
    }
    if (ci_equal(s, "6") || ci_equal(s, "R6")) {
        *out = 6;
        return true;
    }
    return false;
}

static bool parse_fetch_board_or_remote(const char *s, uint8_t *out)
{
    if (!s || !out) return false;
    if (ci_equal(s, "REMOTE") || ci_equal(s, "DEFAULT") || ci_equal(s, "AUTO")) {
        *out = 0;
        return true;
    }
    return parse_board_value(s, out);
}

static bool at_command_preserves_autofetch(const char *arg)
{
    char tmp[48];
    arg = trim_line((char *)arg);
    if (*arg == 0) return false;

    if (toupper((unsigned char)arg[0]) == 'S') {
        char *end = NULL;
        unsigned long reg = strtoul(arg + 1, &end, 10);
        return (reg == 60u || reg == 61u) && end && *end == '?' && end[1] == 0;
    }

    if (toupper((unsigned char)arg[0]) == 'E' &&
        (arg[1] == '0' || arg[1] == '1') &&
        arg[2] == 0) {
        return true;
    }

    if (*arg != '+') return false;
    arg++;
    size_t n = 0;
    while (arg[n] && arg[n] != '=' && arg[n] != '?' && !isspace((unsigned char)arg[n]) && n + 1 < sizeof tmp) {
        tmp[n] = arg[n];
        n++;
    }
    tmp[n] = 0;
    bool query_only = arg[n] == '?' || arg[n] == 0;
    if (!query_only && arg[n] != '=') return false;

    if (ci_equal(tmp, "FETCHSTATUS") || ci_equal(tmp, "AUTOFETCHSTATUS") || ci_equal(tmp, "AFSTATUS")) return true;
    if (ci_equal(tmp, "DOWNLOADSTATUS") || ci_equal(tmp, "DLSTATUS")) return true;
    if (ci_equal(tmp, "FWSTATUS") || ci_equal(tmp, "FIRMWARESTATUS")) return true;
    if (ci_equal(tmp, "THEMESTATUS") || ci_equal(tmp, "WEBTHEMESTATUS")) return true;
    if (ci_equal(tmp, "FETCHNOW") || ci_equal(tmp, "AUTOFETCHNOW") || ci_equal(tmp, "AFNOW")) return true;
    if (ci_equal(tmp, "VERBOSE") || ci_equal(tmp, "WIFIVERBOSE") || ci_equal(tmp, "DEBUG")) return true;
    if (query_only &&
        (ci_equal(tmp, "AUTOFETCH") || ci_equal(tmp, "FETCHINTERVAL") || ci_equal(tmp, "FETCHBOARD"))) {
        return true;
    }
    return false;
}

static void at_settings_reset_defaults(void)
{
    at_settings.autofetch = -1;
    at_settings.fetch_interval_hours = 0;
    at_settings.fetch_board_rev = 0;
    at_settings.verbose = 1;
    remote_http_set_verbose(at_settings.verbose);
    machine_identity_set_name("");
    machine_identity_set_board_rev(0);
    remote_http_autofetch_reset_schedule();
}

static void at_settings_load(void)
{
    if (!ensure_mount_quiet()) return;

    storage_file_t f = {0};
    if (!storage_open(&f, AT_SETTINGS_PATH)) return;
    uint32_t size = storage_size(&f);
    if (size > 1024u) {
        storage_close(&f);
        return;
    }
    char buf[1025];
    size_t got = 0;
    bool ok = storage_read(&f, buf, size, &got);
    storage_close(&f);
    if (!ok || got != size) return;
    buf[size] = 0;

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        line = trim_line(line);
        char *eq = strchr(line, '=');
        if (eq) {
            *eq++ = 0;
            char *key = trim_line(line);
            char *value = trim_line(eq);
            if (ci_equal(key, "autofetch")) {
                bool b = false;
                if (ci_equal(value, "REMOTE") || ci_equal(value, "DEFAULT") || ci_equal(value, "AUTO")) {
                    at_settings.autofetch = -1;
                } else if (parse_bool_value(value, &b)) {
                    at_settings.autofetch = b ? 1 : 0;
                }
            } else if (ci_equal(key, "fetch_interval_hours")) {
                char *end = NULL;
                unsigned long hours = strtoul(value, &end, 10);
                if (!*end && hours >= 3u) at_settings.fetch_interval_hours = (uint32_t)hours;
            } else if (ci_equal(key, "fetch_board") || ci_equal(key, "fetch_board_rev")) {
                uint8_t board = 0;
                if (ci_equal(value, "REMOTE") || ci_equal(value, "DEFAULT") || ci_equal(value, "AUTO")) {
                    at_settings.fetch_board_rev = 0;
                } else if (parse_board_value(value, &board)) {
                    at_settings.fetch_board_rev = board;
                }
                machine_identity_set_board_rev(at_settings.fetch_board_rev);
            } else if (ci_equal(key, "machine") || ci_equal(key, "machine_name") || ci_equal(key, "name")) {
                if (machine_identity_valid_name(value)) {
                    machine_identity_set_name(value);
                }
            } else if (ci_equal(key, "verbose") || ci_equal(key, "wifi_verbose")) {
                char *end = NULL;
                unsigned long v = strtoul(value, &end, 10);
                if (!*end && v <= 2u) at_settings.verbose = (uint8_t)v;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    remote_http_set_verbose(at_settings.verbose);
    machine_identity_set_board_rev(at_settings.fetch_board_rev);
    remote_http_autofetch_reset_schedule();
}

static void sd_backed_config_reload(bool announce)
{
    if (!ensure_mount_quiet()) return;
    at_settings_load();
    if (machine_identity_name()[0]) usb_reconnect_schedule();
    remote_http_init();
    sd_config_loaded = true;

    if (announce && remote_http_verbose() > 0u) {
        uart_cmd_log_puts_best_effort("+SDCARD: mounted config=reloaded transport=");
        uart_cmd_log_puts_best_effort(storage_sd_transport_name());
        uart_cmd_log_puts_best_effort("\r\n");
        uart_cmd_log_puts_best_effort("+REMOTE: ");
        uart_cmd_log_puts_best_effort(remote_http_status());
        uart_cmd_log_puts_best_effort("\r\n");
    }
}

static void sd_hotplug_poll(void)
{
    if (!remote_init_done || sd_config_loaded) return;

    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, sd_hotplug_poll_at) > 0) return;
    sd_hotplug_poll_at = make_timeout_time_ms(M65_SD_HOTPLUG_POLL_MS);

    if (storage_is_mounted() || ensure_mount_quiet()) {
        sd_backed_config_reload(true);
    }
}

static bool at_settings_save(void)
{
    if (!ensure_mount()) return false;
    storage_file_t f = {0};
    if (!storage_open_write(&f, AT_SETTINGS_PATH, true)) {
        uart_cmd_printf("ERROR: AT&W open failed: %s\r\n", storage_last_error());
        return false;
    }
    char buf[192];
    int n = snprintf(buf, sizeof buf,
                     "autofetch=%s\n"
                     "fetch_interval_hours=%lu\n"
                     "fetch_board=%s\n"
                     "verbose=%u\n"
                     "machine_name=%s\n",
                     at_settings.autofetch < 0 ? "remote" : (at_settings.autofetch ? "1" : "0"),
                     (unsigned long)(at_settings.fetch_interval_hours ? at_settings.fetch_interval_hours : 3u),
                     at_settings.fetch_board_rev == 3 ? "3" :
                     at_settings.fetch_board_rev == 6 ? "6" : "remote",
                     (unsigned)at_settings.verbose,
                     machine_identity_name());
    size_t put = 0;
    bool ok = n > 0 && n < (int)sizeof buf &&
              storage_write(&f, buf, (size_t)n, &put) && put == (size_t)n &&
              storage_sync(&f);
    storage_close(&f);
    if (!ok) uart_cmd_printf("ERROR: AT&W write failed: %s\r\n", storage_last_error());
    return ok;
}

static char *parse_filename_arg(char *s, char **rest_out)
{
    s = trim_line(s);
    if (!s || !*s) {
        if (rest_out) *rest_out = s;
        return s;
    }

    if (*s == '"' || *s == '\'') {
        char q = *s++;
        char *name = s;
        char *w = s;
        while (*s && *s != q) {
            if (*s == '\\' && s[1]) s++;
            *w++ = *s++;
        }
        if (*s == q) s++;
        *w = 0;
        if (rest_out) *rest_out = trim_line(s);
        return name;
    }

    char *name = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = 0;
    if (rest_out) *rest_out = trim_line(s);
    return name;
}

static void at_ok(void)
{
    uart_cmd_puts("OK\r\n");
}

static void at_error(const char *msg)
{
    uart_cmd_printf("ERROR: %s\r\n", msg ? msg : "ERROR");
}

typedef enum {
    SDCARD_STATE_NOT_PRESENT = 0,
    SDCARD_STATE_PRESENT,
    SDCARD_STATE_ACTIVE,
} sdcard_state_t;

static sdcard_state_t sdcard_state(void)
{
    if (storage_is_mounted()) {
        return SDCARD_STATE_ACTIVE;
    }
    return storage_sd_may_mount() ? SDCARD_STATE_PRESENT : SDCARD_STATE_NOT_PRESENT;
}

static const char *sdcard_state_human(sdcard_state_t state)
{
    switch (state) {
    case SDCARD_STATE_ACTIVE: return "ACTIVE";
    case SDCARD_STATE_PRESENT: return "PRESENT";
    case SDCARD_STATE_NOT_PRESENT: return "NOT PRESENT";
    default: return "UNKNOWN";
    }
}

static const char *sdcard_state_token(sdcard_state_t state)
{
    switch (state) {
    case SDCARD_STATE_ACTIVE: return "active";
    case SDCARD_STATE_PRESENT: return "present";
    case SDCARD_STATE_NOT_PRESENT: return "not-present";
    default: return "unknown";
    }
}

static void cmd_ati(void)
{
    char identity[40];
    machine_identity_format(identity, sizeof identity);
    uart_cmd_puts("MEGA65 Expansion Board Integrated JTAG v0.1\r\n");
    uart_cmd_puts("EXPERIMENTAL -- SUBJECT TO INTERFACE/API CHANGES\r\n");
    uart_cmd_printf("BUILD: %s\r\n", M65_BUILD_MARKER);
    uart_cmd_printf("MACHINE: %s\r\n", machine_identity_name()[0] ? machine_identity_name() : "(unset)");
    uart_cmd_printf("IDENTITY: %s\r\n", identity);
    uart_cmd_printf("SDCARD: %s\r\n", sdcard_state_human(sdcard_state()));
    uart_cmd_printf("WIFI: %s\r\n", remote_http_wifi_summary());
    at_ok();
}

static void cmd_atd(char *arg)
{
    arg = trim_line(arg);
    if (arg[0] == '*') {
        uart_cmd_puts("BRRING BRRING\r\nNO CARRIER\r\n");
    } else {
        uart_cmd_puts("NO CARRIER\r\n");
    }
}

static void enter_basic_mode(void)
{
    command_mode = CMD_MODE_BASIC;
    uart_cmd_puts("\r\n**** MEGA65 INTEGRATED JTAG V0.1 ****\r\n\r\nREADY.\r\n");
}

static void basic_error(const char *err)
{
    uart_cmd_printf("?%s ERROR\r\nREADY.\r\n", err);
}

static void cmd_help(void)
{
    uart_cmd_puts(
        "+HELP: AT                  modem attention check\n"
        "+HELP: ATE0 / ATE1         disable/enable command echo\n"
        "+HELP: ATI                 identify firmware and WiFi capability\n"
        "+HELP: ATD*                novelty dial command\n"
        "+HELP: AT+GO64             enter BASIC command mode\n"
        "+HELP: GO64                enter BASIC command mode\n"
        "+HELP: AT+VERSION?         firmware version and transport status\n"
        "+HELP: AT+CORELIST[=path]  list numbered .BIT/.COR/.M65J files and dirs\n"
        "+HELP: AT+COREDETAIL[=path] detailed list with COR title/version/board\n"
        "+HELP: AT+COREINFO=file    inspect core file\n"
        "+HELP: AT+CORETEST=file    read core payload from SD and discard\n"
        "+HELP: AT+JTAGLOAD=file|n  hijack JTAG and program core from SD\n"
        "+HELP: AT+JTAGSTREAM=len id stream raw Xilinx payload over serial\n"
        "+HELP: AT+TESTSINK=len     receive/discard bytes; serial throughput test\n"
        "+HELP: AT+FILEWRITE=file len write core file to SD; needs write grant\n"
        "+HELP: AT+FETCH=url [NN]   queue URL fetch to DOWNLOADS/download-NN.dat\n"
        "+HELP: AT+DOWNLOADSTATUS?  show queued URL download status\n"
        "+HELP: AT+DOWNLOADREAD=name read DOWNLOADS/name as raw bytes\n"
        "+HELP: AT+AUTOFETCH[=0|1]  show/set mirror auto-update enable\n"
        "+HELP: AT+FETCHSTATUS?      show mirror auto-update/fetch status\n"
        "+HELP: AT+FETCHNOW[=3|6|remote] fetch mirror manifest now\n"
        "+HELP: AT+FWSTATUS?        show fetched firmware update candidate\n"
        "+HELP: AT+FWUPDATE         install fetched verified firmware update\n"
        "+HELP: AT+THEMESTATUS?     show fetched WWW theme candidate\n"
        "+HELP: AT+THEMEINSTALL     install fetched WWW theme; needs write grant\n"
        "+HELP: AT+FETCHINTERVAL[=hours] show/set auto-update interval; min 3\n"
        "+HELP: AT+FETCHBOARD[=3|6|remote] show/set auto-update board manifest\n"
        "+HELP: AT+VERBOSE[=0|1|2] show/set WiFi/fetch diagnostic verbosity\n"
        "+HELP: AT+MACHINE[=name]  show/set machine name; saved to SD card\n"
        "+HELP: ATS60?              seconds since last successful auto-fetch\n"
        "+HELP: ATS61?              auto-fetch running flag\n"
        "+HELP: AT&W                save AT settings to SD card\n"
        "+HELP: ATZ                 soft reboot Pico and reload saved settings\n"
        "+HELP: AT+WRITEGRANT?      show write-authority status\n"
        "+HELP: AT+REMOTE?          show parsed mega65-jtag.cfg\n"
        "+HELP: AT+WIFI?            show live WiFi/HTTP status\n"
        "+HELP: AT+WIFIPROBE        retry CYW43 hardware probe now\n"
        "+HELP: AT+SDCARD?          show SD card media/mount status\n"
        "+HELP: AT+SDMODE[=auto|hw|soft] show/set SD transport before mount\n"
        "+HELP: AT+JTAGID?          read JTAG IDCODE, using hijack\n"
        "+HELP: AT+JTAGSTATUS?      read Xilinx BOOTSTS/STAT/BYPASS via CFG_OUT\n"
        "+HELP: AT+HIJACK=1|0       manually assert/release JTAG hijack\n"
        "+HELP: AT+MOUNT            mount/remount SD card\n"
        "END\n");
}

static void cmd_atw(void)
{
    if (!require_write_authority("AT&W")) return;
    if (at_settings_save()) at_ok();
}

static void cmd_atz(void)
{
    at_ok();
    sleep_ms(50);
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

static void cmd_s_register(char *arg)
{
    char *end = NULL;
    unsigned long reg = strtoul(arg, &end, 10);
    if (*end != '?' || end[1] != 0) {
        at_error("S REGISTER IS READ ONLY; USE AT+AUTOFETCH OR AT+FETCHINTERVAL");
        return;
    }
    switch (reg) {
    case 60:
        uart_cmd_printf("S60: %lu\r\n", (unsigned long)remote_http_autofetch_last_success_seconds());
        at_ok();
        break;
    case 61:
        uart_cmd_printf("S61: %lu\r\n", (unsigned long)(remote_http_autofetch_running() ? 1u : 0u));
        at_ok();
        break;
    default:
        at_error("UNKNOWN S REGISTER");
        break;
    }
}

static void cmd_version(void)
{
    char identity[40];
    machine_identity_format(identity, sizeof identity);
    uart_cmd_printf("+VERSION: firmware=\"%s\"\r\n", M65_VERSION_STRING);
    uart_cmd_printf("+VERSION: build=%s\r\n", M65_BUILD_MARKER);
    uart_cmd_printf("+VERSION: identity=%s machine=%s board=%s\r\n",
                    identity,
                    machine_identity_name()[0] ? machine_identity_name() : "(unset)",
                    machine_identity_board_token());
    uart_cmd_printf("+VERSION: source=%s\r\n", uart_cmd_source_name(uart_cmd_last_source()));
    uart_cmd_printf("+VERSION: sd_baud=%lu\r\n", (unsigned long)M65_SD_SPI_BAUD);
    uart_cmd_printf("+VERSION: sdcard=%s\r\n", sdcard_state_token(sdcard_state()));
    uart_cmd_printf("+VERSION: sd_transport=%s\r\n", storage_sd_transport_name());
    uart_cmd_printf("+VERSION: write_pin=GP%u write_timeout_ms=%lu write_usb_only=%lu stream_usb_only=%lu\r\n",
                    (unsigned)M65_WRITE_ENABLE_PIN,
                    (unsigned long)M65_WRITE_ENABLE_TIMEOUT_MS,
                    (unsigned long)M65_WRITE_COMMANDS_USB_ONLY,
                    (unsigned long)M65_STREAM_COMMANDS_USB_ONLY);
    uart_cmd_printf("+VERSION: verbose=%u\r\n", (unsigned)at_settings.verbose);
    at_ok();
}

static void cmd_verbose(char *arg, bool have_value)
{
    if (have_value) {
        char *end = NULL;
        unsigned long level = strtoul(arg, &end, 10);
        if (*end || level > 2u) {
            at_error("VERBOSE EXPECTS 0, 1 OR 2");
            return;
        }
        at_settings.verbose = (uint8_t)level;
        remote_http_set_verbose(at_settings.verbose);
    }
    uart_cmd_printf("+VERBOSE: level=%u\r\n", (unsigned)at_settings.verbose);
    at_ok();
}

static void cmd_sd_transport(char *arg)
{
    while (*arg && isspace((unsigned char)*arg)) arg++;
    if (*arg) {
        if (!storage_sd_set_transport(arg)) {
            if (storage_sd_transport_locked()) {
                uart_cmd_puts("ERR D SD transport locked after SD init\n");
            } else {
                uart_cmd_puts("ERR D expected auto|hw|soft\n");
            }
            return;
        }
        storage_sd_probe();
    }

    uart_cmd_printf("OK D %s\n", storage_sd_transport_name());
}

static void cmd_sdcard(void)
{
    sdcard_state_t state = sdcard_state();
    uart_cmd_printf("+SDCARD: state=%s mounted=%lu config_loaded=%lu transport=%s\r\n",
                    sdcard_state_token(state),
                    (unsigned long)(storage_is_mounted() ? 1u : 0u),
                    (unsigned long)(sd_config_loaded ? 1u : 0u),
                    storage_sd_transport_name());
    at_ok();
}

static void cmd_remote(void)
{
    if (!ensure_mount()) return;

    char err[96];
    if (!remote_auth_load(&remote_cfg, err, sizeof err)) {
        uart_cmd_printf("ERROR: REMOTE %s\r\n", err);
        return;
    }

    char ip[24], mask[24], gw[24];
    remote_auth_format_ipv4(remote_cfg.static_ip, ip, sizeof ip);
    remote_auth_format_ipv4(remote_cfg.netmask, mask, sizeof mask);
    remote_auth_format_ipv4(remote_cfg.gateway, gw, sizeof gw);
    uart_cmd_printf("+REMOTE: mode=%s ip=%s netmask=%s gateway=%s ssid=%s http=%lu port=%lu auth=%lu write_grant=%s signatures=%s autofetch=%lu interval_hours=%lu fetch_board=%s channel=%s base_url=%s keys=%lu rules=%lu\r\n",
                    remote_cfg.dhcp ? "dhcp" : "static",
                    ip, mask, gw,
                    remote_cfg.wifi_ssid[0] ? remote_cfg.wifi_ssid : "(unset)",
                    (unsigned long)(remote_cfg.http_enabled ? 1u : 0u),
                    (unsigned long)remote_cfg.http_port,
                    (unsigned long)((remote_cfg.http_user[0] || remote_cfg.http_password[0]) ? 1u : 0u),
                    remote_cfg.require_write_grant ? "required" : "not-required",
                    remote_cfg.require_signatures ? "required" : "optional",
                    (unsigned long)(remote_cfg.autofetch_enabled ? 1u : 0u),
                    (unsigned long)remote_cfg.fetch_interval_hours,
                    core_board_label(remote_cfg.fetch_board_rev),
                    remote_cfg.fetch_channel[0] ? remote_cfg.fetch_channel : "(unset)",
                    remote_cfg.fetch_base_url[0] ? remote_cfg.fetch_base_url : "(unset)",
                    (unsigned long)remote_cfg.trusted_key_count,
                    (unsigned long)remote_cfg.rule_count);

    for (size_t i = 0; i < remote_cfg.rule_count; ++i) {
        char net[24];
        remote_auth_format_ipv4(remote_cfg.rules[i].network, net, sizeof net);
        uart_cmd_printf("+REMOTE_RULE: network=%s mask=%08lx files=%lu bitstreams=%lu\r\n",
                        net,
                        (unsigned long)remote_cfg.rules[i].mask,
                        (unsigned long)(remote_cfg.rules[i].allow_files ? 1u : 0u),
                        (unsigned long)(remote_cfg.rules[i].allow_bitstreams ? 1u : 0u));
    }
    at_ok();
}

static void cmd_wifi(void)
{
    uart_cmd_printf("+WIFI: %s\r\n", remote_http_status());
    uart_cmd_printf("+WIFIDIAG: %s\r\n", remote_http_wifi_diag());
    at_ok();
}

static void cmd_wifi_probe(void)
{
    bool scheduled = remote_http_wifi_probe_now();
    uart_cmd_printf("+WIFIPROBE: scheduled=%lu %s\r\n",
                    (unsigned long)(scheduled ? 1u : 0u),
                    remote_http_wifi_diag());
    at_ok();
}

static void cmd_autofetch(char *arg, bool have_value)
{
    if (have_value) {
        if (!require_write_authority("AF")) return;
        bool b = false;
        if (!parse_bool_value(arg, &b)) {
            at_error("AUTOFETCH EXPECTS 0 OR 1");
            return;
        }
        at_settings.autofetch = b ? 1 : 0;
        remote_http_autofetch_reset_schedule();
    }
    uart_cmd_printf("OK AF override=%d %s\r\n",
                    at_settings.autofetch,
                    remote_http_autofetch_status(at_settings.autofetch, at_settings.fetch_interval_hours, at_settings.fetch_board_rev));
}

static void cmd_fetch_interval(char *arg, bool have_value)
{
    if (have_value) {
        if (!require_write_authority("FI")) return;
        char *end = NULL;
        unsigned long hours = strtoul(arg, &end, 10);
        if (*end || hours < 3u || hours > 8760u) {
            at_error("FETCHINTERVAL MINIMUM IS 3 HOURS");
            return;
        }
        at_settings.fetch_interval_hours = (uint32_t)hours;
        remote_http_autofetch_reset_schedule();
    }
    uart_cmd_printf("OK FI override_hours=%lu %s\r\n",
                    (unsigned long)at_settings.fetch_interval_hours,
                    remote_http_autofetch_status(at_settings.autofetch, at_settings.fetch_interval_hours, at_settings.fetch_board_rev));
}

static void cmd_fetch_board(char *arg, bool have_value)
{
    if (have_value) {
        if (!require_write_authority("FB")) return;
        uint8_t board = 0;
        if (parse_fetch_board_or_remote(arg, &board)) {
            at_settings.fetch_board_rev = board;
            machine_identity_set_board_rev(board);
        } else {
            at_error("FETCHBOARD EXPECTS 3, 6, R3, R6 OR REMOTE");
            return;
        }
        remote_http_autofetch_reset_schedule();
    }
    uart_cmd_printf("OK FB override_board=%s %s\r\n",
                    at_settings.fetch_board_rev ? core_board_label(at_settings.fetch_board_rev) : "remote",
                    remote_http_autofetch_status(at_settings.autofetch, at_settings.fetch_interval_hours, at_settings.fetch_board_rev));
}

static void cmd_machine(char *arg, bool have_value)
{
    if (have_value) {
        if (!require_write_authority("MACHINE")) return;
        arg = trim_line(arg);
        if (!arg[0] || ci_equal(arg, "NONE") || ci_equal(arg, "DEFAULT") || ci_equal(arg, "CLEAR")) {
            machine_identity_set_name("");
        } else if (machine_identity_valid_name(arg)) {
            machine_identity_set_name(arg);
        } else {
            at_error("MACHINE NAME: USE 1-24 ASCII LETTERS, DIGITS, DOT, DASH OR UNDERSCORE");
            return;
        }
        if (!at_settings_save()) return;
    }

    char identity[40];
    machine_identity_format(identity, sizeof identity);
    uart_cmd_printf("+MACHINE: identity=%s board=%s name=",
                    identity,
                    machine_identity_board_token());
    uart_quoted(machine_identity_name());
    uart_cmd_printf(" usb_product=");
    uart_quoted(machine_identity_usb_product());
    uart_cmd_printf(" usb_reenumerate_required=%lu\r\n",
                    (unsigned long)(have_value ? 1u : 0u));
    at_ok();
    if (have_value) usb_reconnect_schedule();
}

static void cmd_fetch_status(void)
{
    uart_cmd_printf("+FETCHSTATUS: %s\r\n",
                    remote_http_autofetch_status(at_settings.autofetch,
                                                 at_settings.fetch_interval_hours,
                                                 at_settings.fetch_board_rev));
    at_ok();
}

static void cmd_fetch_now(char *arg, bool have_value, bool query)
{
    if (query) {
        cmd_fetch_status();
        return;
    }

    uint8_t board = at_settings.fetch_board_rev;
    if (have_value) {
        if (!parse_fetch_board_or_remote(arg, &board)) {
            at_error("FETCHNOW EXPECTS 3, 6, R3, R6 OR REMOTE");
            return;
        }
    }

    bool started = remote_http_autofetch_start_now(at_settings.autofetch,
                                                  at_settings.fetch_interval_hours,
                                                  board);
    uart_cmd_printf("+FETCHNOW: started=%lu %s\r\n",
                    (unsigned long)(started ? 1u : 0u),
                    remote_http_autofetch_status(at_settings.autofetch,
                                                 at_settings.fetch_interval_hours,
                                                 board));
    at_ok();
}

static void cmd_firmware_status(void)
{
    uart_cmd_printf("+FWSTATUS: %s\r\n", remote_http_firmware_status());
    at_ok();
}

static void cmd_firmware_update(void)
{
    if (remote_http_autofetch_running()) {
        at_error("BUSY FETCHING");
        return;
    }
    char err[128];
    if (!remote_http_firmware_update(err, sizeof err)) {
        at_error(err[0] ? err : "FIRMWARE UPDATE FAILED");
        return;
    }
    at_ok();
}

static void cmd_theme_status(void)
{
    uart_cmd_printf("+THEMESTATUS: %s\r\n", remote_http_theme_status());
    at_ok();
}

static void cmd_theme_install(void)
{
    if (remote_http_autofetch_running()) {
        at_error("BUSY FETCHING");
        return;
    }
    if (!require_write_authority("THEMEINSTALL")) return;
    char err[128];
    if (!remote_http_theme_install(err, sizeof err)) {
        at_error(err[0] ? err : "THEME INSTALL FAILED");
        return;
    }
    at_ok();
}

static void cmd_authority(void)
{
    uint32_t remaining = write_gate_remaining_ms();
    uart_cmd_printf("OK A source=%s physical=%lu active=%lu remaining_ms=%lu write_pin=GP%u timeout_ms=%lu write_usb_only=%lu\n",
                    uart_cmd_source_name(uart_cmd_last_source()),
                    (unsigned long)(write_gate_physical_asserted() ? 1u : 0u),
                    (unsigned long)(remaining != 0 ? 1u : 0u),
                    (unsigned long)remaining,
                    (unsigned)M65_WRITE_ENABLE_PIN,
                    (unsigned long)M65_WRITE_ENABLE_TIMEOUT_MS,
                    (unsigned long)M65_WRITE_COMMANDS_USB_ONLY);
}

static void cmd_mount(void)
{
    storage_unmount();
    sd_mounted = false;
    sd_config_loaded = false;
    if (ensure_mount()) {
        if (remote_init_done) {
            sd_backed_config_reload(false);
            uart_cmd_puts("OK M read_only_default=1 config=reloaded\n");
        } else {
            uart_cmd_puts("OK M read_only_default=1 config=deferred\n");
        }
    }
}

static void cmd_list(char *arg)
{
    if (!ensure_mount()) return;
    char *path = unquote_filename(arg);
    if (!path[0]) path = "/";
    uart_cmd_printf("OK L %s\n", path);
    numbered_list_ctx_t ctx = {
        .dir = path,
        .next_number = 1,
    };
    core_list_index_begin();
    if (!storage_list_cores(path, list_cb, &ctx)) {
        core_list_index_begin();
        uart_cmd_printf("ERR L %s\n", storage_last_error());
        return;
    }
    core_list_index_commit();
    uart_cmd_puts("END\n");
}

static void detail_list_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    numbered_list_ctx_t *dl = (numbered_list_ctx_t *)ctx;
    uint16_t number = dl ? dl->next_number++ : 0;
    char path[256];
    if (!make_child_path(dl && dl->dir ? dl->dir : "/", name, path, sizeof path)) {
        uart_cmd_printf("+COREERR: index=%lu path-too-long name=", (unsigned long)number);
        uart_quoted(name);
        uart_cmd_puts("\n");
        return;
    }

    bool is_partial = is_partial_core_path(path);
    if (number) {
        (void)core_list_index_add(number, dl && dl->dir ? dl->dir : "/", name, is_dir, is_partial);
    }

    if (is_dir) {
        uart_cmd_printf("+COREDIR: index=%lu size=%lu path=",
                        (unsigned long)number,
                        (unsigned long)size);
        uart_quoted(path);
        uart_cmd_puts("\n");
        return;
    }
    if (is_partial) {
        uart_cmd_printf("+COREPARTIAL: index=%lu size=%lu path=",
                        (unsigned long)number,
                        (unsigned long)size);
        uart_quoted(path);
        uart_cmd_puts(" status=\"download in progress\"\n");
        return;
    }

    core_file_t cf;
    if (!core_open(&cf, path)) {
        uart_cmd_printf("+COREERR: index=%lu size=%lu path=",
                        (unsigned long)number,
                        (unsigned long)size);
        uart_quoted(path);
        uart_cmd_puts(" error=");
        uart_quoted(core_last_error());
        uart_cmd_puts("\n");
        return;
    }

    uint8_t board = core_effective_board(&cf, path);
    uart_cmd_printf("+COREDETAIL: index=%lu size=%lu kind=%s board=%s board_id=%lu payload_offset=%lu payload_length=%lu expected_idcode=%08lx path=",
                    (unsigned long)number,
                    (unsigned long)size,
                    core_kind_name(cf.kind),
                    core_board_label(board),
                    (unsigned long)board,
                    (unsigned long)cf.payload_offset,
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);
    uart_quoted(path);
    uart_cmd_puts(" title=");
    uart_quoted(cf.title[0] ? cf.title : "");
    uart_cmd_puts(" version=");
    uart_quoted(cf.version[0] ? cf.version : "");
    uart_cmd_puts(" model=");
    uart_quoted(cf.model[0] ? cf.model : "");
    uart_cmd_printf(" model_id=%lu\n", (unsigned long)cf.model_id);
    core_close(&cf);
}

static void cmd_detail_list(char *arg)
{
    if (!ensure_mount()) return;
    char *path = unquote_filename(arg);
    if (!path[0]) path = "/";
    uart_cmd_printf("OK LD %s\n", path);
    numbered_list_ctx_t ctx = {
        .dir = path,
        .next_number = 1,
    };
    core_list_index_begin();
    if (!storage_list_cores(path, detail_list_cb, &ctx)) {
        core_list_index_begin();
        uart_cmd_printf("ERR LD %s\n", storage_last_error());
        return;
    }
    core_list_index_commit();
    uart_cmd_puts("END\n");
}

static void cmd_info(char *arg)
{
    if (!ensure_mount()) return;
    char *name = unquote_filename(arg);
    if (!name[0]) {
        uart_cmd_puts("ERR I missing filename\n");
        return;
    }

    core_file_t cf;
    if (!core_open(&cf, name)) {
        uart_cmd_printf("ERR I %s\n", core_last_error());
        return;
    }

    uint8_t board = core_effective_board(&cf, name);
    uart_cmd_printf("OK I %s kind=%s board=%s board_id=%lu payload_offset=%lu payload_length=%lu expected_idcode=%08lx title=",
                    cf.path,
                    core_kind_name(cf.kind),
                    core_board_label(board),
                    (unsigned long)board,
                    (unsigned long)cf.payload_offset,
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);
    uart_quoted(cf.title[0] ? cf.title : "");
    uart_cmd_puts(" version=");
    uart_quoted(cf.version[0] ? cf.version : "");
    uart_cmd_puts(" model=");
    uart_quoted(cf.model[0] ? cf.model : "");
    uart_cmd_printf(" model_id=%lu\n", (unsigned long)cf.model_id);
    core_close(&cf);
}

static void cmd_jtag_id(void)
{
    jtag_hijack_claim();
    uint32_t id = jtag_read_idcode();
    jtag_hijack_release();
    if (id == 0x00000000u || id == 0xffffffffu) {
        uart_cmd_printf("WARN J IDCODE %08lx looks like a JTAG bus error; check hijack, power, TDO and pinout\n",
                        (unsigned long)id);
    }
    uart_cmd_printf("OK J %08lx\n", (unsigned long)id);
}

static void cmd_xilinx_status(void)
{
    jtag_status_t st;
    jtag_hijack_claim();
    bool ok = jtag_read_xilinx_status(&st);
    jtag_hijack_release();
    if (ok) print_xilinx_status("OK X", &st);
    else uart_cmd_printf("ERR X %s\n", jtag_last_error());
}

static void cmd_hijack(char *arg)
{
    arg = trim_line(arg);
    if (arg[0] == '1') {
        jtag_hijack_claim();
        uart_cmd_puts("OK H 1\n");
    } else if (arg[0] == '0') {
        jtag_hijack_release();
        uart_cmd_puts("OK H 0\n");
    } else {
        uart_cmd_puts("ERR H expected 0 or 1\n");
    }
}

static bool idcode_match_main(uint32_t seen, uint32_t expected)
{
    if (expected == 0) return true;
    return (seen & 0x0fffffffu) == (expected & 0x0fffffffu);
}

static bool idcode_suspicious(uint32_t id)
{
    return id == 0x00000000u || id == 0xffffffffu;
}

static void warn_suspicious_idcode(const char *tag, uint32_t id)
{
    uart_cmd_printf("WARN %s IDCODE %08lx looks like a JTAG bus error; check hijack, power, TDO and pinout\n",
                    tag ? tag : "JTAG",
                    (unsigned long)id);
}

typedef struct {
    uint32_t timeout_ms;
} serial_stream_ctx_t;

static bool serial_stream_reader(void *ctx, uint8_t *buf, size_t len, size_t *got)
{
    serial_stream_ctx_t *ss = (serial_stream_ctx_t *)ctx;
    uint32_t timeout_ms = ss ? ss->timeout_ms : 5000u;
    bool ok = uart_cmd_read_bytes(buf, len, timeout_ms);
    if (got) *got = ok ? len : 0;
    return ok;
}

static void cmd_sink_stream(char *arg)
{
    arg = trim_line(arg);
    if (!arg[0]) {
        uart_cmd_puts("ERR N expected: N <payload_length>\n");
        return;
    }

    char *end = NULL;
    uint32_t payload_length = (uint32_t)strtoul(arg, &end, 0);
    if (payload_length == 0) {
        uart_cmd_puts("ERR N zero length\n");
        return;
    }

    uart_cmd_printf("OK N READY length=%lu\n", (unsigned long)payload_length);

    static uint8_t buf[8192];
    uint32_t remaining = payload_length;
    uint32_t done = 0;
    absolute_time_t start = get_absolute_time();
    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        if (!uart_cmd_read_bytes(buf, want, 5000u)) {
            uart_cmd_printf("ERR N short read at %lu/%lu\n",
                            (unsigned long)done,
                            (unsigned long)payload_length);
            return;
        }
        remaining -= (uint32_t)want;
        done += (uint32_t)want;
        if ((done & 0x3ffffu) == 0 || done == payload_length) {
            uart_cmd_printf("SINK %lu/%lu\n", (unsigned long)done, (unsigned long)payload_length);
        }
    }

    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    if (us <= 0) us = 1;
    uint32_t kib_s = (uint32_t)(((uint64_t)payload_length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK N DONE bytes=%lu time_us=%lld rate_KiBps=%lu\n",
                    (unsigned long)payload_length,
                    (long long)us,
                    (unsigned long)kib_s);
}

static void cmd_stream_program(char *arg)
{
    if (!stream_command_source_allowed()) {
        uart_cmd_printf("ERR S raw stream programming requires USB source; source=%s\n",
                        uart_cmd_source_name(uart_cmd_last_source()));
        return;
    }

    arg = trim_line(arg);
    if (!arg[0]) {
        uart_cmd_puts("ERR S expected: S <payload_length> <expected_idcode_hex>\n");
        return;
    }

    char *end = NULL;
    uint32_t payload_length = (uint32_t)strtoul(arg, &end, 0);
    end = trim_line(end);
    uint32_t expected_idcode = 0;
    if (end && end[0]) expected_idcode = (uint32_t)strtoul(end, NULL, 16);

    if (payload_length == 0) {
        uart_cmd_puts("ERR S zero length\n");
        return;
    }

    jtag_hijack_claim();
    uint32_t id = jtag_read_idcode();
    jtag_hijack_release();
    if (idcode_suspicious(id)) {
        warn_suspicious_idcode("S", id);
    }
    if (expected_idcode != 0) {
        if (!idcode_match_main(id, expected_idcode)) {
            uart_cmd_printf("ERR S IDCODE mismatch: saw %08lx expected %08lx\n",
                            (unsigned long)id,
                            (unsigned long)expected_idcode);
            return;
        }
    }

    uart_cmd_printf("OK S READY length=%lu expected_idcode=%08lx source=%s\n",
                    (unsigned long)payload_length,
                    (unsigned long)expected_idcode,
                    uart_cmd_source_name(uart_cmd_last_source()));

    serial_stream_ctx_t reader_ctx = { .timeout_ms = 5000u };
    jtag_program_options_t opts = {
        .check_idcode = false,
        .use_hijack = true,
        .release_after = true,
        .progress_cb = progress_cb,
        .progress_ctx = NULL,
    };

    activity_led_put(true);
    bool ok = jtag_program_stream(payload_length,
                                  expected_idcode,
                                  serial_stream_reader,
                                  &reader_ctx,
                                  &opts);
    activity_led_put(false);

    if (ok) {
        jtag_status_t st;
        if (jtag_get_last_status(&st)) print_xilinx_status("XSTAT S", &st);
        uart_cmd_puts("OK S DONE\n");
    } else {
        uart_cmd_printf("ERR S %s\n", jtag_last_error());
    }
}

static void cmd_test_read(char *arg)
{
    if (!ensure_mount()) return;
    char *name = unquote_filename(arg);
    if (!name[0]) {
        uart_cmd_puts("ERR T missing filename\n");
        return;
    }

    core_file_t cf;
    if (!core_open(&cf, name)) {
        uart_cmd_printf("ERR T %s\n", core_last_error());
        return;
    }

    uart_cmd_printf("OK T OPEN %s kind=%s payload_offset=%lu payload_length=%lu expected_idcode=%08lx\n",
                    cf.path,
                    core_kind_name(cf.kind),
                    (unsigned long)cf.payload_offset,
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);

    if (!core_rewind_payload(&cf)) {
        uart_cmd_printf("ERR T rewind failed: %s\n", storage_last_error());
        core_close(&cf);
        return;
    }

    static uint8_t buf[8192];
    uint32_t remaining = cf.payload_length;
    uint32_t done = 0;
    absolute_time_t start = get_absolute_time();

    activity_led_put(true);
    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        size_t got = 0;
        if (!core_read_payload(&cf, buf, want, &got)) {
            activity_led_put(false);
            uart_cmd_printf("ERR T read failed at %lu/%lu: %s\n",
                            (unsigned long)done,
                            (unsigned long)cf.payload_length,
                            storage_last_error());
            core_close(&cf);
            return;
        }
        if (got == 0) {
            activity_led_put(false);
            uart_cmd_printf("ERR T short read at %lu/%lu\n",
                            (unsigned long)done,
                            (unsigned long)cf.payload_length);
            core_close(&cf);
            return;
        }
        done += (uint32_t)got;
        remaining -= (uint32_t)got;
        if ((done & 0x3ffffu) == 0 || done == cf.payload_length) {
            uart_cmd_printf("READ %lu/%lu\n",
                            (unsigned long)done,
                            (unsigned long)cf.payload_length);
        }
    }
    activity_led_put(false);

    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    if (us <= 0) us = 1;
    uint32_t kib_s = (uint32_t)(((uint64_t)cf.payload_length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK T DONE bytes=%lu time_us=%lld rate_KiBps=%lu\n",
                    (unsigned long)cf.payload_length,
                    (long long)us,
                    (unsigned long)kib_s);

    core_close(&cf);
}

static bool parse_core_list_number(const char *s, uint16_t *number_out)
{
    s = trim_line((char *)s);
    if (!s || !s[0]) return false;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }

    char *end = NULL;
    unsigned long n = strtoul(s, &end, 10);
    if (!end || *end || n == 0 || n > 65535u) return false;
    if (number_out) *number_out = (uint16_t)n;
    return true;
}

static const char *path_basename(const char *path)
{
    path = path ? path : "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *core_list_resolve_bare_name(const char *name)
{
    if (!core_list_index_valid || !name || !name[0]) return NULL;
    if (strchr(name, '/') || strchr(name, '\\')) return NULL;

    const char *folded_match = NULL;
    bool folded_ambiguous = false;

    for (uint16_t i = 0; i < core_list_index_count; i++) {
        const core_list_index_entry_t *entry = &core_list_index[i];
        if (entry->is_dir || entry->is_partial) continue;

        const char *path = core_list_index_path(entry);
        const char *base = path_basename(path);
        if (strcmp(base, name) == 0) return path;
        if (ci_equal(base, name)) {
            if (folded_match && strcmp(folded_match, path) != 0) {
                folded_ambiguous = true;
            } else {
                folded_match = path;
            }
        }
    }

    return folded_ambiguous ? NULL : folded_match;
}

static bool resolve_program_target(char *name, const char **path_out)
{
    uint16_t number = 0;
    if (parse_core_list_number(name, &number)) {
        if (!core_list_index_valid) {
            uart_cmd_puts("ERR P no numbered core list; run AT+CORELIST first\n");
            return false;
        }

        const core_list_index_entry_t *entry = core_list_index_find(number);
        if (!entry) {
            uart_cmd_printf("ERR P no core list entry %lu\n", (unsigned long)number);
            return false;
        }
        if (entry->is_dir) {
            uart_cmd_printf("ERR P core list entry %lu is a directory\n", (unsigned long)number);
            return false;
        }
        if (entry->is_partial) {
            uart_cmd_printf("ERR P core list entry %lu is a partial download\n", (unsigned long)number);
            return false;
        }

        *path_out = core_list_index_path(entry);
        return true;
    }

    const char *listed_path = core_list_resolve_bare_name(name);
    *path_out = listed_path ? listed_path : name;
    return true;
}

static void cmd_program(char *arg)
{
    if (!ensure_mount()) return;
    char *name = unquote_filename(arg);
    if (!name[0]) {
        uart_cmd_puts("ERR P missing filename\n");
        return;
    }

    const char *path = name;
    if (!resolve_program_target(name, &path)) return;

    core_file_t cf;
    if (!core_open(&cf, path)) {
        uart_cmd_printf("ERR P %s\n", core_last_error());
        return;
    }

    uart_cmd_printf("OK P OPEN %s kind=%s length=%lu expected_idcode=%08lx\n",
                    cf.path,
                    core_kind_name(cf.kind),
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);

    jtag_hijack_claim();
    uint32_t id = jtag_read_idcode();
    jtag_hijack_release();
    if (idcode_suspicious(id)) {
        warn_suspicious_idcode("P", id);
    }

    jtag_program_options_t opts = {
        .check_idcode = true,
        .use_hijack = true,
        .release_after = true,
        .progress_cb = progress_cb,
        .progress_ctx = NULL,
    };

    activity_led_put(true);
    bool ok = jtag_program_core(&cf, &opts);
    activity_led_put(false);

    if (ok) {
        jtag_status_t st;
        if (jtag_get_last_status(&st)) print_xilinx_status("XSTAT P", &st);
        uart_cmd_puts("OK P DONE\n");
    } else {
        uart_cmd_printf("ERR P %s\n", jtag_last_error());
    }

    core_close(&cf);
}

typedef struct {
    uint8_t board_rev;
} basic_list_ctx_t;

static void basic_list_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    basic_list_ctx_t *bl = (basic_list_ctx_t *)ctx;
    if (!is_dir && is_partial_core_path(name)) return;
    if (!is_dir && bl && (bl->board_rev == 3 || bl->board_rev == 6)) {
        core_file_t cf;
        if (!core_open(&cf, name)) return;
        bool match = core_matches_board(&cf, name, bl->board_rev);
        core_close(&cf);
        if (!match) return;
    }

    uart_cmd_printf("%5lu \"%s\" %s\r\n",
                    (unsigned long)((size + 255u) / 256u),
                    name,
                    is_dir ? "DIR" : "PRG");
}

static void basic_load_directory(uint8_t board_rev)
{
    if (!ensure_mount_quiet()) {
        basic_error("DEVICE NOT PRESENT");
        return;
    }

    uart_cmd_puts("SEARCHING FOR $\r\nLOADING\r\n");
    basic_list_ctx_t ctx = { .board_rev = board_rev };
    if (!storage_list_cores("/", basic_list_cb, &ctx)) {
        basic_error("FILE NOT FOUND");
        return;
    }
    uart_cmd_puts("READY.\r\n");
}

static void basic_load_core(const char *name, uint8_t board_rev)
{
    if (!name || !name[0]) {
        basic_error("SYNTAX");
        return;
    }
    if (!ensure_mount_quiet()) {
        basic_error("DEVICE NOT PRESENT");
        return;
    }

    core_file_t cf;
    if (!core_open(&cf, name)) {
        basic_error("FILE NOT FOUND");
        return;
    }
    if (!core_matches_board(&cf, name, board_rev)) {
        core_close(&cf);
        basic_error("FILE NOT FOUND");
        return;
    }

    uart_cmd_printf("SEARCHING FOR %s\r\nLOADING\r\n", name);

    jtag_program_options_t opts = {
        .check_idcode = true,
        .use_hijack = true,
        .release_after = true,
        .progress_cb = progress_cb,
        .progress_ctx = NULL,
    };

    activity_led_put(true);
    bool ok = jtag_program_core(&cf, &opts);
    activity_led_put(false);
    core_close(&cf);

    if (ok) {
        uart_cmd_puts("READY.\r\n");
    } else {
        basic_error("LOAD");
    }
}

static bool parse_basic_load_name(char *s, char **name_out, uint8_t *board_out, bool *secondary1_out)
{
    s = trim_line(s);
    if (!ci_starts_with(s, "LOAD")) return false;
    s += 4;
    s = trim_line(s);
    if (*s != '"') return false;
    s++;

    char *name = s;
    while (*s && *s != '"') s++;
    if (*s != '"') return false;
    *s++ = 0;

    s = trim_line(s);
    if (*s != ',') return false;
    s++;
    s = trim_line(s);
    char *end = NULL;
    unsigned long board = strtoul(s, &end, 10);
    if (end == s || (board != 3 && board != 6)) return false;
    s = end;
    s = trim_line(s);
    bool secondary1 = false;
    if (*s) {
        if (*s != ',') return false;
        s++;
        s = trim_line(s);
        if (*s != '1') return false;
        s++;
        secondary1 = true;
        s = trim_line(s);
        if (*s) return false;
    }

    *name_out = name;
    if (board_out) *board_out = (uint8_t)board;
    if (secondary1_out) *secondary1_out = secondary1;
    return true;
}

static void dispatch_basic(char *line)
{
    char *s = trim_line(line);
    if (!s[0]) return;

    if (ci_equal(s, "SYS64738")) {
        command_mode = CMD_MODE_AT;
        at_ok();
        return;
    }

    char *name = NULL;
    uint8_t board_rev = 0;
    bool secondary1 = false;
    if (parse_basic_load_name(s, &name, &board_rev, &secondary1)) {
        if (ci_equal(name, "$")) basic_load_directory(board_rev);
        else if (secondary1) basic_load_core(name, board_rev);
        else basic_error("SYNTAX");
        return;
    }

    basic_error("SYNTAX");
}

static void cmd_write_file(char *arg)
{
    if (!ensure_mount()) return;
    if (!require_write_authority("W")) return;

    char *rest = NULL;
    char *name = parse_filename_arg(arg, &rest);
    if (!name || !name[0] || !rest || !rest[0]) {
        uart_cmd_puts("ERR W expected: W <file.bit|file.cor|file.m65j> <length>\n");
        return;
    }
    if (!safe_core_write_path(name)) {
        uart_cmd_puts("ERR W unsafe path or unsupported extension; allowed: .bit .cor .m65j, no '..'\n");
        return;
    }

    char *end = NULL;
    uint32_t length = (uint32_t)strtoul(rest, &end, 0);
    if (length == 0) {
        uart_cmd_puts("ERR W zero length\n");
        return;
    }

    char tmp_path[256];
    if (snprintf(tmp_path, sizeof tmp_path, "%s.partial", name) >= (int)sizeof tmp_path) {
        uart_cmd_puts("ERR W path too long\n");
        return;
    }

    storage_delete(tmp_path);

    storage_file_t out = {0};
    if (!storage_open_write(&out, tmp_path, true)) {
        uart_cmd_printf("ERR W open failed: %s\n", storage_last_error());
        return;
    }

    uart_cmd_printf("OK W READY path=%s tmp=%s length=%lu source=%s remaining_ms=%lu\n",
                    name,
                    tmp_path,
                    (unsigned long)length,
                    uart_cmd_source_name(uart_cmd_last_source()),
                    (unsigned long)write_gate_remaining_ms());

    static uint8_t buf[8192];
    uint32_t remaining = length;
    uint32_t done = 0;
    absolute_time_t start = get_absolute_time();
    bool ok = true;

    activity_led_put(true);
    while (remaining) {
        if (!write_gate_active()) {
            uart_cmd_printf("ERR W write authority expired at %lu/%lu\n",
                            (unsigned long)done, (unsigned long)length);
            ok = false;
            break;
        }
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        if (!uart_cmd_read_bytes(buf, want, 5000u)) {
            uart_cmd_printf("ERR W short receive at %lu/%lu\n",
                            (unsigned long)done, (unsigned long)length);
            ok = false;
            break;
        }
        size_t put = 0;
        if (!storage_write(&out, buf, want, &put) || put != want) {
            uart_cmd_printf("ERR W write failed at %lu/%lu: %s\n",
                            (unsigned long)done, (unsigned long)length, storage_last_error());
            ok = false;
            break;
        }
        done += (uint32_t)want;
        remaining -= (uint32_t)want;
        if ((done & 0x3ffffu) == 0 || done == length) {
            uart_cmd_printf("WRITE %lu/%lu\n", (unsigned long)done, (unsigned long)length);
        }
    }

    if (ok && !storage_sync(&out)) {
        uart_cmd_printf("ERR W sync failed: %s\n", storage_last_error());
        ok = false;
    }
    storage_close(&out);
    activity_led_put(false);

    if (!ok) {
        storage_delete(tmp_path);
        return;
    }

    // FatFs f_rename usually fails if the destination exists. Delete the final
    // file only after the temporary upload is complete and synced.
    storage_delete(name);
    if (!storage_rename(tmp_path, name)) {
        uart_cmd_printf("ERR W rename failed: %s\n", storage_last_error());
        storage_delete(tmp_path);
        return;
    }

    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    if (us <= 0) us = 1;
    uint32_t kib_s = (uint32_t)(((uint64_t)length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK W DONE path=%s bytes=%lu time_us=%lld rate_KiBps=%lu remaining_ms=%lu\n",
                    name,
                    (unsigned long)length,
                    (long long)us,
                    (unsigned long)kib_s,
                    (unsigned long)write_gate_remaining_ms());
}

static void cmd_fetch(char *arg)
{
    if (!ensure_mount()) return;

    char *rest = NULL;
    char *url = parse_filename_arg(arg, &rest);
    char *slot_arg = parse_filename_arg(rest ? rest : (char *)"", NULL);
    if (!url || !url[0]) {
        uart_cmd_puts("ERR F expected: F <http://url> [slot 00-FF]\n");
        return;
    }
    int slot = -1;
    if (slot_arg && slot_arg[0] && !parse_download_slot_token(slot_arg, &slot)) {
        uart_cmd_puts("ERR F optional destination must be slot 00-FF; files are DOWNLOADS/download-NN.dat\n");
        return;
    }

    char err[128];
    char dest[64];
    bool ok = remote_http_download_start(url, slot, dest, sizeof dest, err, sizeof err);
    if (!ok) {
        uart_cmd_printf("ERR F %s\n", err[0] ? err : "fetch failed");
        return;
    }
    uart_cmd_printf("+DOWNLOAD: queued url=%s path=%s\n", url, dest);
    uart_cmd_printf("+DOWNLOADSTATUS: %s\n", remote_http_download_status());
    at_ok();
}

static void cmd_download_status(void)
{
    uart_cmd_printf("+DOWNLOADSTATUS: %s\r\n", remote_http_download_status());
    at_ok();
}

static void cmd_read_download(char *arg)
{
    if (!ensure_mount()) return;
    char *name = unquote_filename(arg);
    char path[256];
    if (!name[0] || !make_download_path(name, path, sizeof path)) {
        uart_cmd_puts("ERR R expected safe DOWNLOADS filename\n");
        return;
    }

    storage_file_t f = {0};
    if (!storage_open(&f, path)) {
        uart_cmd_printf("ERR R open failed: %s\n", storage_last_error());
        return;
    }

    uint32_t size = storage_size(&f);
    uart_cmd_printf("OK R READY path=%s length=%lu\n", path, (unsigned long)size);

    static uint8_t buf[1024];
    uint32_t done = 0;
    while (done < size) {
        size_t want = size - done;
        if (want > sizeof buf) want = sizeof buf;
        size_t got = 0;
        if (!storage_read(&f, buf, want, &got) || got == 0) {
            storage_close(&f);
            uart_cmd_printf("\nERR R read failed at %lu/%lu: %s\n",
                            (unsigned long)done,
                            (unsigned long)size,
                            storage_last_error());
            return;
        }
        uart_cmd_write_bytes(buf, got);
        done += (uint32_t)got;
    }
    storage_close(&f);
    uart_cmd_puts("\nOK R DONE\n");
}

#if M65_ENABLE_LEGACY_UART_COMMANDS
static bool dispatch_legacy(char cmd, char *arg)
{
    switch (cmd) {
    case 'V': cmd_version(); break;
    case 'L': cmd_list(arg); break;
    case 'I': cmd_info(arg); break;
    case 'T': cmd_test_read(arg); break;
    case 'P': cmd_program(arg); break;
    case 'S': cmd_stream_program(arg); break;
    case 'N': cmd_sink_stream(arg); break;
    case 'W': cmd_write_file(arg); break;
    case 'F': cmd_fetch(arg); break;
    case 'R': cmd_read_download(arg); break;
    case 'A': cmd_authority(); break;
    case 'D': cmd_sd_transport(arg); break;
    case 'J': cmd_jtag_id(); break;
    case 'X': cmd_xilinx_status(); break;
    case 'H': cmd_hijack(arg); break;
    case 'M': cmd_mount(); break;
    case '?': cmd_help(); break;
    default:
        return false;
    }
    return true;
}
#endif

static void dispatch_at(char *arg)
{
    arg = trim_line(arg);
    if (!arg[0]) {
        at_ok();
        return;
    }

    if (ci_equal(arg, "E0")) {
        uart_cmd_set_echo(false);
        at_ok();
        return;
    }

    if (ci_equal(arg, "E1")) {
        uart_cmd_set_echo(true);
        at_ok();
        return;
    }

    if (ci_equal(arg, "I")) {
        cmd_ati();
        return;
    }

    if (ci_equal(arg, "Z")) {
        cmd_atz();
        return;
    }

    if (ci_equal(arg, "&W")) {
        cmd_atw();
        return;
    }

    if (toupper((unsigned char)arg[0]) == 'S') {
        cmd_s_register(arg + 1);
        return;
    }

    if (toupper((unsigned char)arg[0]) == 'D') {
        cmd_atd(arg + 1);
        return;
    }

    if (ci_equal(arg, "+GO64")) {
        enter_basic_mode();
        return;
    }

    if (arg[0] != '+') {
        at_error("SYNTAX");
        return;
    }

    char *cmd = arg + 1;
    char *value = strchr(cmd, '=');
    char *query = strchr(cmd, '?');
    char *cmd_end = value ? value : query;
    if (!cmd_end) cmd_end = cmd + strlen(cmd);
    char saved = *cmd_end;
    *cmd_end = 0;
    char *param = value ? trim_line(value + 1) : (char *)"";

    if (ci_equal(cmd, "HELP")) {
        cmd_help();
    } else if (ci_equal(cmd, "I") || ci_equal(cmd, "IDENT")) {
        cmd_ati();
    } else if (ci_equal(cmd, "VERSION") || ci_equal(cmd, "VER")) {
        cmd_version();
    } else if (ci_equal(cmd, "CORELIST") || ci_equal(cmd, "LIST")) {
        cmd_list(param);
    } else if (ci_equal(cmd, "COREDETAIL") || ci_equal(cmd, "CORELS") || ci_equal(cmd, "DETAIL")) {
        cmd_detail_list(param);
    } else if (ci_equal(cmd, "COREINFO") || ci_equal(cmd, "CORE") || ci_equal(cmd, "INFO")) {
        cmd_info(param);
    } else if (ci_equal(cmd, "CORETEST") || ci_equal(cmd, "TESTREAD")) {
        cmd_test_read(param);
    } else if (ci_equal(cmd, "JTAGLOAD") || ci_equal(cmd, "PROGRAM") || ci_equal(cmd, "LOAD")) {
        cmd_program(param);
    } else if (ci_equal(cmd, "JTAGSTREAM") || ci_equal(cmd, "STREAM")) {
        cmd_stream_program(param);
    } else if (ci_equal(cmd, "TESTSINK") || ci_equal(cmd, "SINK")) {
        cmd_sink_stream(param);
    } else if (ci_equal(cmd, "FILEWRITE") || ci_equal(cmd, "WRITE")) {
        cmd_write_file(param);
    } else if (ci_equal(cmd, "FETCH") || ci_equal(cmd, "HTTPFETCH") || ci_equal(cmd, "DOWNLOAD")) {
        cmd_fetch(param);
    } else if (ci_equal(cmd, "DOWNLOADSTATUS") || ci_equal(cmd, "DLSTATUS")) {
        cmd_download_status();
    } else if (ci_equal(cmd, "DOWNLOADREAD") || ci_equal(cmd, "READ")) {
        cmd_read_download(param);
    } else if (ci_equal(cmd, "AUTOFETCH")) {
        cmd_autofetch(param, value != NULL);
    } else if (ci_equal(cmd, "FETCHSTATUS") || ci_equal(cmd, "AUTOFETCHSTATUS") || ci_equal(cmd, "AFSTATUS")) {
        cmd_fetch_status();
    } else if (ci_equal(cmd, "FETCHNOW") || ci_equal(cmd, "AUTOFETCHNOW") || ci_equal(cmd, "AFNOW")) {
        cmd_fetch_now(param, value != NULL, query != NULL);
    } else if (ci_equal(cmd, "FWSTATUS") || ci_equal(cmd, "FIRMWARESTATUS")) {
        cmd_firmware_status();
    } else if (ci_equal(cmd, "FWUPDATE") || ci_equal(cmd, "FIRMWAREUPDATE")) {
        cmd_firmware_update();
    } else if (ci_equal(cmd, "THEMESTATUS") || ci_equal(cmd, "WEBTHEMESTATUS")) {
        cmd_theme_status();
    } else if (ci_equal(cmd, "THEMEINSTALL") || ci_equal(cmd, "WEBTHEMEINSTALL")) {
        cmd_theme_install();
    } else if (ci_equal(cmd, "FETCHINTERVAL")) {
        cmd_fetch_interval(param, value != NULL);
    } else if (ci_equal(cmd, "FETCHBOARD")) {
        cmd_fetch_board(param, value != NULL);
    } else if (ci_equal(cmd, "VERBOSE") || ci_equal(cmd, "WIFIVERBOSE") || ci_equal(cmd, "DEBUG")) {
        cmd_verbose(param, value != NULL);
    } else if (ci_equal(cmd, "MACHINE") || ci_equal(cmd, "MACHINENAME") ||
               ci_equal(cmd, "NAME") || ci_equal(cmd, "IDENTITY")) {
        cmd_machine(param, value != NULL);
    } else if (ci_equal(cmd, "WRITEGRANT") || ci_equal(cmd, "AUTH")) {
        cmd_authority();
    } else if (ci_equal(cmd, "REMOTE")) {
        cmd_remote();
    } else if (ci_equal(cmd, "WIFI") || ci_equal(cmd, "HTTP")) {
        cmd_wifi();
    } else if (ci_equal(cmd, "WIFIPROBE")) {
        cmd_wifi_probe();
    } else if (ci_equal(cmd, "SDCARD") || ci_equal(cmd, "SDSTATUS")) {
        cmd_sdcard();
    } else if (ci_equal(cmd, "SDMODE")) {
        cmd_sd_transport(value ? param : (char *)"");
    } else if (ci_equal(cmd, "JTAGID")) {
        cmd_jtag_id();
    } else if (ci_equal(cmd, "JTAGSTATUS") || ci_equal(cmd, "XSTATUS")) {
        cmd_xilinx_status();
    } else if (ci_equal(cmd, "HIJACK")) {
        cmd_hijack(param);
    } else if (ci_equal(cmd, "MOUNT")) {
        cmd_mount();
    } else {
        at_error("UNKNOWN COMMAND");
    }

    *cmd_end = saved;
}

static void dispatch(char *line)
{
    char *s = trim_line(line);
    if (!s[0]) return;
    remote_http_defer_wifi_recovery(M65_WIFI_COMMAND_QUIET_MS);

    if (command_mode == CMD_MODE_BASIC) {
        dispatch_basic(s);
        return;
    }

    if (ci_equal(s, "GO64")) {
        remote_http_autofetch_cancel("uart command");
        enter_basic_mode();
        return;
    }

    if (ci_starts_with(s, "AT")) {
        if (!at_command_preserves_autofetch(s + 2)) {
            remote_http_autofetch_cancel("uart command");
        }
        dispatch_at(s + 2);
        return;
    }

#if M65_ENABLE_LEGACY_UART_COMMANDS
    remote_http_autofetch_cancel("uart command");
    char cmd = (char)toupper((unsigned char)s[0]);
    char *arg = trim_line(s + 1);
    if (!dispatch_legacy(cmd, arg)) {
        at_error("UNKNOWN COMMAND");
    }
#else
    at_error("SYNTAX");
#endif
}

int main(void)
{
#if M65_BOOT_DIAG
    boot_diag_uart_init();
    boot_diag_pin_init();
    boot_diag_stage(1, "main-enter");
#endif

    machine_identity_init_defaults();
    boot_diag_stage(2, "identity-defaults");

    stdio_init_all();
    boot_diag_stage(3, "stdio-init");

    sleep_ms(200);

    activity_led_init();
    boot_diag_stage(4, "activity-led");

    write_gate_init();
    boot_diag_stage(5, "writegate");

    jtag_gpio_init();
    boot_diag_stage(6, "jtag-init");

    boot_diag_stage(7, "sd-probe-start");
    storage_sd_probe();
    boot_diag_stage(8, "sd-probe-done");

    boot_diag_stage(9, "remote-boot-check-start");
    remote_http_boot_check();
    boot_diag_stage(0, "remote-boot-check-done");

    boot_diag_stage(1, "switching-uart-to-2000000");
    uart_cmd_init();

    uart_cmd_printf("OK BOOT %s\n", M65_VERSION_STRING);
    uart_cmd_printf("OK SD %s\n", storage_sd_transport_name());
    uart_cmd_puts("OK READY\n");

    absolute_time_t remote_init_at = make_timeout_time_ms(M65_REMOTE_INIT_DELAY_MS);
    sd_hotplug_poll_at = make_timeout_time_ms(M65_SD_HOTPLUG_POLL_MS);

    char line[256];
    for (;;) {
        bootsel_button_poll();
        if (uart_cmd_read_line(line, sizeof line)) {
            dispatch(line);
            continue;
        }
        if (!remote_init_done &&
            absolute_time_diff_us(get_absolute_time(), remote_init_at) <= 0) {
            if (ensure_mount_quiet()) {
                sd_backed_config_reload(false);
            } else {
                at_settings_load();
                if (machine_identity_name()[0]) usb_reconnect_schedule();
                remote_http_init();
                sd_config_loaded = false;
            }
            uart_cmd_printf("OK REMOTE %s\n", remote_http_status());
            remote_init_done = true;
        }
        sd_hotplug_poll();
        remote_http_poll();
        if (remote_init_done) {
            remote_http_autofetch_poll(at_settings.autofetch, at_settings.fetch_interval_hours, at_settings.fetch_board_rev);
        }
        usb_reconnect_poll();
        tight_loop_contents();
    }
}
