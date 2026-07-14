#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void remote_http_init(void);
void remote_http_poll(void);
bool remote_http_active(void);
const char *remote_http_status(void);
bool remote_http_fetch_to_downloads(const char *url, const char *name, char *err, size_t err_len);
void remote_http_autofetch_poll(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override);
void remote_http_autofetch_reset_schedule(void);
void remote_http_autofetch_cancel(const char *reason);
const char *remote_http_autofetch_status(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override);
uint32_t remote_http_autofetch_last_success_seconds(void);
bool remote_http_autofetch_running(void);
