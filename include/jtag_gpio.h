#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_file.h"

typedef void (*jtag_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);
typedef bool (*jtag_stream_read_cb_t)(void *ctx, uint8_t *buf, size_t len, size_t *got);

typedef struct {
    bool check_idcode;
    bool use_hijack;
    bool release_after;
    bool read_done_pin;
    jtag_progress_cb_t progress_cb;
    void *progress_ctx;
} jtag_program_options_t;

void jtag_gpio_init(void);
void jtag_hijack_claim(void);
void jtag_hijack_release(void);
uint32_t jtag_read_idcode(void);
bool jtag_program_core(core_file_t *cf, const jtag_program_options_t *opts);
bool jtag_program_stream(uint32_t payload_length,
                         uint32_t expected_idcode,
                         jtag_stream_read_cb_t read_cb,
                         void *read_ctx,
                         const jtag_program_options_t *opts);
const char *jtag_last_error(void);
