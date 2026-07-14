#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_file.h"

typedef void (*jtag_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);
typedef bool (*jtag_stream_read_cb_t)(void *ctx, uint8_t *buf, size_t len, size_t *got);

typedef struct {
    uint32_t idcode;
    uint32_t bootsts;
    uint32_t stat;
    uint32_t bypass;
    bool bootsts_valid;
    bool stat_valid;
    bool bypass_valid;
} jtag_status_t;

typedef struct {
    bool check_idcode;
    bool use_hijack;
    bool release_after;
    jtag_progress_cb_t progress_cb;
    void *progress_ctx;
} jtag_program_options_t;

typedef struct {
    uint32_t payload_length;
    uint32_t expected_idcode;
    uint32_t done;
    bool active;
    bool release_after;
    jtag_progress_cb_t progress_cb;
    void *progress_ctx;
} jtag_stream_writer_t;

void jtag_gpio_init(void);
void jtag_hijack_claim(void);
void jtag_hijack_release(void);
uint32_t jtag_read_idcode(void);
bool jtag_read_xilinx_status(jtag_status_t *st);
bool jtag_get_last_status(jtag_status_t *st);
bool jtag_program_core(core_file_t *cf, const jtag_program_options_t *opts);
bool jtag_program_stream(uint32_t payload_length,
                         uint32_t expected_idcode,
                         jtag_stream_read_cb_t read_cb,
                         void *read_ctx,
                         const jtag_program_options_t *opts);
bool jtag_program_writer_begin(jtag_stream_writer_t *wr,
                               uint32_t payload_length,
                               uint32_t expected_idcode,
                               const jtag_program_options_t *opts);
bool jtag_program_writer_write(jtag_stream_writer_t *wr, const uint8_t *buf, size_t len);
bool jtag_program_writer_finish(jtag_stream_writer_t *wr);
void jtag_program_writer_abort(jtag_stream_writer_t *wr);
const char *jtag_last_error(void);
