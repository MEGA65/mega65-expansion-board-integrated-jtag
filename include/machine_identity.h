#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M65_MACHINE_NAME_MAX 24u

void machine_identity_init_defaults(void);
bool machine_identity_valid_name(const char *name);
void machine_identity_set_name(const char *name);
const char *machine_identity_name(void);

void machine_identity_set_board_rev(uint8_t board_rev);
uint8_t machine_identity_board_rev(void);
const char *machine_identity_board_token(void);

void machine_identity_format(char *out, size_t out_len);
const char *machine_identity_usb_product(void);
