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
#include "jtag_gpio.h"

static bool sd_mounted = false;

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

static void list_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    (void)ctx;
    uart_cmd_printf("%s %lu %s\n", is_dir ? "DIR" : "CORE", (unsigned long)size, name);
}

static void cmd_help(void)
{
    uart_cmd_puts(
        "OK HELP\n"
        "V                         version\n"
        "L [path]                  list .BIT/.COR/.M65J files and dirs\n"
        "I <file>                  inspect core file\n"
        "P <file>                  hijack JTAG and program core from SD/FAT\n"
        "S <length> <idcode>        stream raw Xilinx payload over this serial link\n"
        "N <length>                 receive/discard bytes; serial throughput test\n"
        "J                         read JTAG IDCODE, using hijack\n"
        "H 1|0                     manually assert/release JTAG hijack\n"
        "M                         mount/remount SD card\n"
        "?                         help\n"
        "END\n");
}

static void cmd_version(void)
{
    uart_cmd_printf("OK V %s\n", M65_VERSION_STRING);
}

static void cmd_mount(void)
{
    storage_unmount();
    sd_mounted = false;
    if (ensure_mount()) uart_cmd_puts("OK M\n");
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

    uart_cmd_printf("OK I %s kind=%s payload_offset=%lu payload_length=%lu expected_idcode=%08lx\n",
                    cf.path,
                    core_kind_name(cf.kind),
                    (unsigned long)cf.payload_offset,
                    (unsigned long)cf.payload_length,
                    (unsigned long)cf.expected_idcode);
    core_close(&cf);
}

static void cmd_jtag_id(void)
{
    jtag_hijack_claim();
    uint32_t id = jtag_read_idcode();
    jtag_hijack_release();
    uart_cmd_printf("OK J %08lx\n", (unsigned long)id);
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

    // Check the chain before asking the host to dump megabytes of binary data.
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

    uart_cmd_printf("OK S READY length=%lu expected_idcode=%08lx\n",
                    (unsigned long)payload_length,
                    (unsigned long)expected_idcode);

    serial_stream_ctx_t reader_ctx = { .timeout_ms = 5000u };
    jtag_program_options_t opts = {
        .check_idcode = false,       // Already checked above, before binary transfer.
        .use_hijack = true,
        .release_after = true,
        .read_done_pin = false,      // Minimum wiring is just GND+TCK+TMS+TDI+TDO.
        .progress_cb = progress_cb,
        .progress_ctx = NULL,
    };

    gpio_put(M65_LED_PIN, 1);
    bool ok = jtag_program_stream(payload_length,
                                  expected_idcode,
                                  serial_stream_reader,
                                  &reader_ctx,
                                  &opts);
    gpio_put(M65_LED_PIN, 0);

    if (ok) {
        uart_cmd_puts("OK S DONE\n");
    } else {
        uart_cmd_printf("ERR S %s\n", jtag_last_error());
    }
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
        .read_done_pin = true,
        .progress_cb = progress_cb,
        .progress_ctx = NULL,
    };

    gpio_put(M65_LED_PIN, 1);
    bool ok = jtag_program_core(&cf, &opts);
    gpio_put(M65_LED_PIN, 0);

    if (ok) {
        uart_cmd_puts("OK P DONE\n");
    } else {
        uart_cmd_printf("ERR P %s\n", jtag_last_error());
    }

    core_close(&cf);
}

static void dispatch(char *line)
{
    char *s = trim_line(line);
    if (!s[0]) return;

    char cmd = (char)toupper((unsigned char)s[0]);
    char *arg = trim_line(s + 1);

    switch (cmd) {
    case 'V': cmd_version(); break;
    case 'L': cmd_list(arg); break;
    case 'I': cmd_info(arg); break;
    case 'P': cmd_program(arg); break;
    case 'S': cmd_stream_program(arg); break;
    case 'N': cmd_sink_stream(arg); break;
    case 'J': cmd_jtag_id(); break;
    case 'H': cmd_hijack(arg); break;
    case 'M': cmd_mount(); break;
    case '?': cmd_help(); break;
    default:
        uart_cmd_printf("ERR unknown command '%c'\n", cmd);
        break;
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(200);

    gpio_init(M65_LED_PIN);
    gpio_set_dir(M65_LED_PIN, GPIO_OUT);
    gpio_put(M65_LED_PIN, 0);

    uart_cmd_init();
    jtag_gpio_init();

    uart_cmd_printf("OK BOOT %s\n", M65_VERSION_STRING);
    uart_cmd_puts("OK READY\n");

    char line[256];
    for (;;) {
        if (uart_cmd_read_line(line, sizeof line)) {
            dispatch(line);
        }
    }
}
