#pragma once

// Interface between the bootloader's startup code (main.c) and the OTA
// install logic (ota_install.c), which is kept free of hardware startup code
// so it can also be exercised on a PC (tests/ota_test.c).

#include <stdbool.h>
#include <stdint.h>

#include "ota_record.h"

#define XIP_BASE_ADDR 0x10000000u

static inline uint32_t slot0_base(void)
{
    return XIP_BASE_ADDR + (uint32_t)M65_BOOTLOADER_SLOT0_OFFSET;
}

// main.c
void diag_puts(const char *s);
void diag_stage(unsigned stage, const char *label);
bool image_vectors_look_valid(uint32_t image_base, uint32_t link_base);

// ota_install.c: installs a pending update, if there is one. Returns with
// slot 0 holding either the new image or, if the update was abandoned, the
// image it held before.
void ota_boot_check(bool legacy_request);
