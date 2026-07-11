#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void uart_cmd_init(void);
bool uart_cmd_read_line(char *buf, size_t buflen);

// Read raw bytes from the same command port that supplied the most recent
// completed line. This is used by the streaming-program command so binary
// payload bytes are not confused with the other command port.
bool uart_cmd_read_bytes(uint8_t *buf, size_t len, uint32_t timeout_ms);

void uart_cmd_puts(const char *s);
void uart_cmd_printf(const char *fmt, ...);

char *trim_line(char *s);
char *unquote_filename(char *s);
