#pragma once

#include <stdbool.h>
#include <stdint.h>

void write_gate_init(void);
void write_gate_refresh_from_pin(void);
bool write_gate_physical_asserted(void);
bool write_gate_active(void);
uint32_t write_gate_remaining_ms(void);

