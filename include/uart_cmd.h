#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    UART_CMD_SRC_UART = 0,
    UART_CMD_SRC_USB  = 1,
} uart_cmd_source_t;

void uart_cmd_init(void);
bool uart_cmd_read_line(char *buf, size_t buflen);
bool uart_cmd_read_bytes(uint8_t *buf, size_t len, uint32_t timeout_ms);
void uart_cmd_write_bytes(const uint8_t *buf, size_t len);
void uart_cmd_puts(const char *s);
void uart_cmd_printf(const char *fmt, ...);
void uart_cmd_log_puts_best_effort(const char *s);
void uart_cmd_log_printf_best_effort(const char *fmt, ...);
char *trim_line(char *s);
char *unquote_filename(char *s);
uart_cmd_source_t uart_cmd_last_source(void);
const char *uart_cmd_source_name(uart_cmd_source_t src);
