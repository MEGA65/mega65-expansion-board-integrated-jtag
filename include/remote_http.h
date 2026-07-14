#pragma once

#include <stdbool.h>
#include <stddef.h>

void remote_http_init(void);
void remote_http_poll(void);
bool remote_http_active(void);
const char *remote_http_status(void);
bool remote_http_fetch_to_downloads(const char *url, const char *name, char *err, size_t err_len);
