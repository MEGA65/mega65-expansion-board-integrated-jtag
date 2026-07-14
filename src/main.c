#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "config.h"
#include "uart_cmd.h"
#include "storage.h"
#include "core_file.h"
#include "core_filter.h"
#include "jtag_gpio.h"
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
#ifndef M65_WRITE_COMMANDS_USB_ONLY
#define M65_WRITE_COMMANDS_USB_ONLY 1
#endif
#ifndef M65_STREAM_COMMANDS_USB_ONLY
#define M65_STREAM_COMMANDS_USB_ONLY 1
#endif

static bool sd_mounted = false;
static remote_auth_config_t remote_cfg;

typedef enum {
    CMD_MODE_AT = 0,
    CMD_MODE_BASIC,
} command_mode_t;

static command_mode_t command_mode = CMD_MODE_AT;

static void progress_cb(uint32_t done, uint32_t total, void *ctx)
{
    (void)ctx;
    uart_cmd_printf("PROG %lu/%lu\n", (unsigned long)done, (unsigned long)total);
}

static bool ensure_mount(void)
{
    if (sd_mounted) return true;
    if (!storage_mount()) {
        uart_cmd_printf("ERR SD %s\n", storage_last_error());
        return false;
    }
    sd_mounted = true;
    return true;
}

static bool ensure_mount_quiet(void)
{
    if (sd_mounted) return true;
    if (!storage_mount()) return false;
    sd_mounted = true;
    return true;
}

static void list_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    (void)ctx;
    uart_cmd_printf("%s %lu %s\n", is_dir ? "DIR" : "CORE", (unsigned long)size, name);
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

static bool safe_core_write_path(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 220) return false; // leaves room for .tmp and FatFs path buffer
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

static void cmd_ati(void)
{
    uart_cmd_puts("MEGA65 Expansion Board Integrated JTAG v0.1\r\n");
    uart_cmd_puts("EXPERIMENTAL -- SUBJECT TO INTERFACE/API CHANGES\r\n");
#if M65_WIFI_SUPPORTED
    uart_cmd_puts("WIFI: SUPPORTED\r\n");
#else
    uart_cmd_puts("WIFI: NOT SUPPORTED\r\n");
#endif
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
        "+HELP: ATI                 identify firmware and WiFi capability\n"
        "+HELP: ATD*                novelty dial command\n"
        "+HELP: AT+GO64             enter BASIC command mode\n"
        "+HELP: GO64                enter BASIC command mode\n"
        "+HELP: AT+VERSION?         firmware version and transport status\n"
        "+HELP: AT+CORELIST[=path]  list .BIT/.COR/.M65J files and dirs\n"
        "+HELP: AT+COREINFO=file    inspect core file\n"
        "+HELP: AT+CORETEST=file    read core payload from SD and discard\n"
        "+HELP: AT+JTAGLOAD=file    hijack JTAG and program core from SD\n"
        "+HELP: AT+JTAGSTREAM=len id stream raw Xilinx payload over serial\n"
        "+HELP: AT+TESTSINK=len     receive/discard bytes; serial throughput test\n"
        "+HELP: AT+FILEWRITE=file len write core file to SD; needs write grant\n"
        "+HELP: AT+FETCH=url name   fetch http:// URL into DOWNLOADS/name\n"
        "+HELP: AT+DOWNLOADREAD=name read DOWNLOADS/name as raw bytes\n"
        "+HELP: AT+WRITEGRANT?      show write-authority status\n"
        "+HELP: AT+REMOTE?          show parsed REMOTE_ENABLE.cfg\n"
        "+HELP: AT+SDMODE[=auto|hw|soft] show/set SD transport before mount\n"
        "+HELP: AT+JTAGID?          read JTAG IDCODE, using hijack\n"
        "+HELP: AT+JTAGSTATUS?      read Xilinx BOOTSTS/STAT/BYPASS via CFG_OUT\n"
        "+HELP: AT+HIJACK=1|0       manually assert/release JTAG hijack\n"
        "+HELP: AT+MOUNT            mount/remount SD card\n"
        "END\n");
}

static void cmd_version(void)
{
    uart_cmd_printf("OK V %s source=%s sd_baud=%lu sd_transport=%s write_pin=GP%u write_timeout_ms=%lu write_usb_only=%lu stream_usb_only=%lu\n",
                    M65_VERSION_STRING,
                    uart_cmd_source_name(uart_cmd_last_source()),
                    (unsigned long)M65_SD_SPI_BAUD,
                    storage_sd_transport_name(),
                    (unsigned)M65_WRITE_ENABLE_PIN,
                    (unsigned long)M65_WRITE_ENABLE_TIMEOUT_MS,
                    (unsigned long)M65_WRITE_COMMANDS_USB_ONLY,
                    (unsigned long)M65_STREAM_COMMANDS_USB_ONLY);
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
    uart_cmd_printf("+REMOTE: mode=%s ip=%s netmask=%s gateway=%s ssid=%s http=%lu port=%lu auth=%lu write_grant=%s signatures=%s keys=%lu rules=%lu\r\n",
                    remote_cfg.dhcp ? "dhcp" : "static",
                    ip, mask, gw,
                    remote_cfg.wifi_ssid[0] ? remote_cfg.wifi_ssid : "(unset)",
                    (unsigned long)(remote_cfg.http_enabled ? 1u : 0u),
                    (unsigned long)remote_cfg.http_port,
                    (unsigned long)((remote_cfg.http_user[0] || remote_cfg.http_password[0]) ? 1u : 0u),
                    remote_cfg.require_write_grant ? "required" : "not-required",
                    remote_cfg.require_signatures ? "required" : "optional",
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
    if (ensure_mount()) uart_cmd_puts("OK M read_only_default=1\n");
}

static void cmd_list(char *arg)
{
    if (!ensure_mount()) return;
    char *path = unquote_filename(arg);
    if (!path[0]) path = "/";
    uart_cmd_printf("OK L %s\n", path);
    if (!storage_list_cores(path, list_cb, NULL)) {
        uart_cmd_printf("ERR L %s\n", storage_last_error());
        return;
    }
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

    uart_cmd_printf("OK I %s kind=%s payload_offset=%lu payload_length=%lu expected_idcode=%08lx model_id=%lu model=%s\n",
                    cf.path,
                    core_kind_name(cf.kind),
                    (unsigned long)cf.payload_offset,
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode,
                    (unsigned long)cf.model_id,
                    cf.model[0] ? cf.model : "(unknown)");
    core_close(&cf);
}

static void cmd_jtag_id(void)
{
    jtag_hijack_claim();
    uint32_t id = jtag_read_idcode();
    jtag_hijack_release();
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
    uint32_t kb_s = (uint32_t)(((uint64_t)payload_length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK N DONE bytes=%lu time_us=%lld rate_kBps=%lu\n",
                    (unsigned long)payload_length,
                    (long long)us,
                    (unsigned long)kb_s);
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

    if (expected_idcode != 0) {
        jtag_hijack_claim();
        uint32_t id = jtag_read_idcode();
        jtag_hijack_release();
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
    uint32_t kb_s = (uint32_t)(((uint64_t)cf.payload_length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK T DONE bytes=%lu time_us=%lld rate_kBps=%lu\n",
                    (unsigned long)cf.payload_length,
                    (long long)us,
                    (unsigned long)kb_s);

    core_close(&cf);
}

static void cmd_program(char *arg)
{
    if (!ensure_mount()) return;
    char *name = unquote_filename(arg);
    if (!name[0]) {
        uart_cmd_puts("ERR P missing filename\n");
        return;
    }

    core_file_t cf;
    if (!core_open(&cf, name)) {
        uart_cmd_printf("ERR P %s\n", core_last_error());
        return;
    }

    uart_cmd_printf("OK P OPEN %s kind=%s length=%lu expected_idcode=%08lx\n",
                    cf.path,
                    core_kind_name(cf.kind),
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);

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
    if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp", name) >= (int)sizeof tmp_path) {
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
    uint32_t kb_s = (uint32_t)(((uint64_t)length * 1000000ull) / ((uint64_t)us * 1024ull));
    uart_cmd_printf("OK W DONE path=%s bytes=%lu time_us=%lld rate_kBps=%lu remaining_ms=%lu\n",
                    name,
                    (unsigned long)length,
                    (long long)us,
                    (unsigned long)kb_s,
                    (unsigned long)write_gate_remaining_ms());
}

static void cmd_fetch(char *arg)
{
    if (!ensure_mount()) return;
    if (!require_write_authority("F")) return;

    char *rest = NULL;
    char *url = parse_filename_arg(arg, &rest);
    char *name = parse_filename_arg(rest ? rest : (char *)"", NULL);
    if (!url || !url[0] || !name || !name[0]) {
        uart_cmd_puts("ERR F expected: F <http://url> <downloads-name>\n");
        return;
    }
    if (!safe_download_name(name)) {
        uart_cmd_puts("ERR F unsafe DOWNLOADS filename\n");
        return;
    }

    char err[128];
    uart_cmd_printf("OK F FETCHING url=%s name=%s\n", url, name);
    activity_led_put(true);
    bool ok = remote_http_fetch_to_downloads(url, name, err, sizeof err);
    activity_led_put(false);
    if (!ok) {
        uart_cmd_printf("ERR F %s\n", err[0] ? err : "fetch failed");
        return;
    }
    uart_cmd_printf("OK F DONE path=DOWNLOADS/%s\n", name);
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

    if (ci_equal(arg, "I")) {
        cmd_ati();
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
    } else if (ci_equal(cmd, "DOWNLOADREAD") || ci_equal(cmd, "READ")) {
        cmd_read_download(param);
    } else if (ci_equal(cmd, "WRITEGRANT") || ci_equal(cmd, "AUTH")) {
        cmd_authority();
    } else if (ci_equal(cmd, "REMOTE")) {
        cmd_remote();
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

    if (command_mode == CMD_MODE_BASIC) {
        dispatch_basic(s);
        return;
    }

    if (ci_equal(s, "GO64")) {
        enter_basic_mode();
        return;
    }

    if (ci_starts_with(s, "AT")) {
        dispatch_at(s + 2);
        return;
    }

#if M65_ENABLE_LEGACY_UART_COMMANDS
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
    stdio_init_all();
    sleep_ms(200);

    activity_led_init();

    write_gate_init();
    uart_cmd_init();
    jtag_gpio_init();
    storage_sd_probe();
    remote_http_init();

    uart_cmd_printf("OK BOOT %s\n", M65_VERSION_STRING);
    uart_cmd_printf("OK SD %s\n", storage_sd_transport_name());
    if (remote_http_active()) {
        uart_cmd_printf("OK REMOTE %s\n", remote_http_status());
    }
    uart_cmd_puts("OK READY\n");

    char line[256];
    for (;;) {
        remote_http_poll();
        if (uart_cmd_read_line(line, sizeof line)) {
            dispatch(line);
        }
        tight_loop_contents();
    }
}
