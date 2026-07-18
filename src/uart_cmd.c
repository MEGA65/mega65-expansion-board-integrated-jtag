#include "uart_cmd.h"
#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/error.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"

#ifndef M65_ENABLE_USB_CDC
#define M65_ENABLE_USB_CDC 1
#endif

static uart_cmd_source_t last_cmd_source = UART_CMD_SRC_UART;

void uart_cmd_init(void)
{
    uart_init(M65_UART_ID, M65_UART_BAUD);
    gpio_set_function(M65_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(M65_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(M65_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(M65_UART_ID, false, false);
    uart_set_fifo_enabled(M65_UART_ID, true);
}

static bool feed_line_char(char c, char *buf, size_t buflen, size_t *pos)
{
    if (c == '\r') return false;
    if (c == '\n') {
        buf[*pos] = 0;
        *pos = 0;
        return true;
    }
    if (*pos + 1 < buflen) {
        buf[(*pos)++] = c;
    } else {
        // Overflow: drop the partial line. The eventual newline returns blank.
        *pos = 0;
    }
    return false;
}

bool uart_cmd_read_line(char *buf, size_t buflen)
{
    static size_t uart_pos = 0;
    static size_t usb_pos = 0;

    if (!buf || buflen == 0) return false;

    while (uart_is_readable(M65_UART_ID)) {
        char c = (char)uart_getc(M65_UART_ID);
        if (feed_line_char(c, buf, buflen, &uart_pos)) {
            last_cmd_source = UART_CMD_SRC_UART;
            return true;
        }
    }

#if M65_ENABLE_USB_CDC
    // Pull USB CDC/stdin data in chunks through the Pico SDK stdio layer.
    // This avoids the byte-at-a-time getchar_timeout_us() path without
    // taking ownership of TinyUSB descriptors directly.
    char tmp[128];
    int n = 0;
    while ((n = stdio_get_until(tmp, (int)sizeof tmp, get_absolute_time())) > 0) {
        for (int i = 0; i < n; i++) {
            if (feed_line_char(tmp[i], buf, buflen, &usb_pos)) {
                last_cmd_source = UART_CMD_SRC_USB;
                return true;
            }
        }
    }
#endif

    return false;
}

static bool read_uart_bytes_bulk(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    size_t pos = 0;

    while (pos < len) {
        while (pos < len && uart_is_readable(M65_UART_ID)) {
            buf[pos++] = (uint8_t)uart_getc(M65_UART_ID);
            deadline = make_timeout_time_ms(timeout_ms); // timeout is an inter-byte gap timeout
        }
        if (pos >= len) break;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return false;
        tight_loop_contents();
    }

    return true;
}

static bool read_usb_bytes_bulk(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
#if M65_ENABLE_USB_CDC
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    size_t pos = 0;

    while (pos < len) {
        size_t want = len - pos;
        if (want > 8192) want = 8192;

        int n = stdio_get_until((char *)(buf + pos), (int)want, deadline);
        if (n > 0) {
            pos += (size_t)n;
            deadline = make_timeout_time_ms(timeout_ms); // timeout is a quiet-gap timeout
            continue;
        }

        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return false;
        tight_loop_contents();
    }

    return true;
#else
    (void)buf; (void)len; (void)timeout_ms;
    return false;
#endif
}

bool uart_cmd_read_bytes(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!buf && len) return false;
    if (len == 0) return true;

    if (last_cmd_source == UART_CMD_SRC_USB) {
        return read_usb_bytes_bulk(buf, len, timeout_ms);
    }
    return read_uart_bytes_bulk(buf, len, timeout_ms);
}

void uart_cmd_write_bytes(const uint8_t *buf, size_t len)
{
    if (!buf || !len) return;
    if (last_cmd_source == UART_CMD_SRC_USB) {
#if M65_ENABLE_USB_CDC
        fwrite(buf, 1, len, stdout);
        fflush(stdout);
#endif
        return;
    }
    uart_write_blocking(M65_UART_ID, buf, len);
}

void uart_cmd_puts(const char *s)
{
    if (!s) return;

    // Broadcast replies to both command ports. That is deliberately simple:
    // native MEGA65-side UART control and PC-side USB CDC both see the same log.
    uart_puts(M65_UART_ID, s);
#if M65_ENABLE_USB_CDC
    // pico_enable_stdio_usb() routes stdout to USB CDC. Keep replies simple;
    // only the binary receive path needs chunking.
    fputs(s, stdout);
    fflush(stdout);
#endif
}

void uart_cmd_printf(const char *fmt, ...)
{
    char tmp[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    uart_cmd_puts(tmp);
}

void uart_cmd_log_puts_best_effort(const char *s)
{
    if (!s) return;
    size_t len = strlen(s);

    for (size_t i = 0; i < len && uart_is_writable(M65_UART_ID); i++) {
        uart_putc_raw(M65_UART_ID, s[i]);
    }

#if M65_ENABLE_USB_CDC
    // Background status lines must never pin the main loop behind a CDC flush.
    // The Pico SDK path has bounded timeouts, and we deliberately do not
    // fflush() here. Dropped/truncated progress chatter is better than a wedged
    // network stack or command parser.
    stdio_put_string(s, (int)len, false, false);
#endif
}

void uart_cmd_log_printf_best_effort(const char *fmt, ...)
{
    char tmp[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    uart_cmd_log_puts_best_effort(tmp);
}

uart_cmd_source_t uart_cmd_last_source(void)
{
    return last_cmd_source;
}

const char *uart_cmd_source_name(uart_cmd_source_t src)
{
    switch (src) {
    case UART_CMD_SRC_USB: return "USB";
    case UART_CMD_SRC_UART: return "UART";
    default: return "UNKNOWN";
    }
}

char *trim_line(char *s)
{
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

char *unquote_filename(char *s)
{
    s = trim_line(s);
    if (!s || !*s) return s;
    if (*s == '"' || *s == '\'') {
        char q = *s++;
        char *w = s;
        char *r = s;
        while (*r && *r != q) {
            if (*r == '\\' && r[1]) r++;
            *w++ = *r++;
        }
        *w = 0;
        return s;
    }
    return s;
}
