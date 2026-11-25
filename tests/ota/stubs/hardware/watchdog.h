#pragma once
#include <stdint.h>
typedef struct { uint32_t scratch[8]; } watchdog_hw_t;
extern watchdog_hw_t *watchdog_hw;
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
