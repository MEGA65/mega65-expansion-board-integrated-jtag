#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool firmware_update_available(void);
bool remote_http_firmware_update(char *err, size_t err_len);
void remote_http_firmware_reboot(uint32_t delay_ms);
bool remote_http_verify_firmware_package(const char *path, char *err, size_t err_len);
