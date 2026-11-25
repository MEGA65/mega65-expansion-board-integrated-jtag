#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void remote_http_init(void);
void remote_http_boot_check(void);
void remote_http_poll(void);
void remote_http_defer_wifi_recovery(uint32_t quiet_ms);
void remote_http_set_verbose(uint8_t level);
uint8_t remote_http_verbose(void);
bool remote_http_active(void);
const char *remote_http_status(void);
const char *remote_http_wifi_summary(void);
const char *remote_http_wifi_diag(void);
bool remote_http_wifi_probe_now(void);
bool remote_http_download_start(const char *url, int slot, char *dest, size_t dest_len, char *err, size_t err_len);
const char *remote_http_download_status(void);
void remote_http_autofetch_poll(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override);
void remote_http_autofetch_reset_schedule(void);
void remote_http_autofetch_cancel(const char *reason);
void remote_http_sd_removed(void);
bool remote_http_autofetch_start_now(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override);
const char *remote_http_autofetch_status(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override);
uint32_t remote_http_autofetch_last_success_seconds(void);
bool remote_http_autofetch_running(void);
const char *remote_http_firmware_status(void);
bool firmware_update_available(void);
// Checks the staged package and writes it to flash slot 1 with an install
// record. Does not reboot; on success call remote_http_firmware_reboot().
bool remote_http_firmware_update(char *err, size_t err_len);
// Restarts into the bootloader (which installs the update) after delay_ms.
void remote_http_firmware_reboot(uint32_t delay_ms);
// Random, non-zero, chosen at each power-up or restart (src/main.c). Reported
// in /sd-state and AT+SDCARD? so the web page and the MEGA65 core loader can
// tell that the Pico restarted, e.g. after a firmware update: the SD change
// counter restarts too and could land on the value they last saw.
uint32_t m65_boot_id(void);
// Checks the package's signed trailer, following the require_signatures policy.
bool remote_http_verify_firmware_package(const char *path, char *err, size_t err_len);
const char *remote_http_theme_status(void);
bool remote_http_theme_install(char *err, size_t err_len);
bool remote_http_theme_install_named(const char *theme_name, char *err, size_t err_len);
