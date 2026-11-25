// Host stand-in for the Pico SDK, just enough for src/ota_apply.c and
// bootloader/ota_install.c. Flash is a real mapping at XIP_BASE (see
// ota_test.c), so pointer reads of flash behave as on the Pico.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define XIP_BASE  0x10000000u
#define SRAM_BASE 0x20000000u
#if PICO_RP2040
#define SRAM_END  0x20042000u
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#else
#define SRAM_END  0x20082000u
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)
#endif

typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
static inline alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t cb, void *ud, bool past)
{ (void)ms; (void)cb; (void)ud; (void)past; return 1; }
static inline void tight_loop_contents(void) {}
