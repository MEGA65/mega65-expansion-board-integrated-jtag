#include "jtag_gpio.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

// Xilinx 7-series 6-bit JTAG instructions.
#define XC7_IR_LEN       6u
#define XC7_IR_IDCODE    0x09u
#define XC7_IR_CFG_IN    0x05u
#define XC7_IR_JPROGRAM  0x0bu
#define XC7_IR_JSTART    0x0cu
#define XC7_IR_ISC_NOOP  0x14u
#define XC7_IR_BYPASS    0x3fu

static char last_err[128];

static const uint32_t tck_mask = (1u << M65_JTAG_TCK_PIN);
static const uint32_t tms_mask = (1u << M65_JTAG_TMS_PIN);
static const uint32_t tdi_mask = (1u << M65_JTAG_TDI_PIN);
static const uint32_t tdo_mask = (1u << M65_JTAG_TDO_PIN);

static void set_err(const char *msg)
{
    snprintf(last_err, sizeof(last_err), "%s", msg);
}

const char *jtag_last_error(void)
{
    return last_err[0] ? last_err : "no JTAG error";
}

static inline void jtag_delay(void)
{
#if M65_JTAG_DELAY_LOOPS > 0
    for (volatile unsigned i = 0; i < M65_JTAG_DELAY_LOOPS; i++) {
        __asm volatile("nop");
    }
#endif
}

static inline bool tick(bool tms, bool tdi)
{
    // Fast direct-SIO GPIO writes. This avoids the SDK gpio_put()/gpio_get()
    // function-call overhead in the JTAG hot path. TCK is assumed low on entry
    // and is driven low again before return.
    uint32_t set_mask = 0;
    uint32_t clr_mask = 0;

    if (tms) set_mask |= tms_mask; else clr_mask |= tms_mask;
    if (tdi) set_mask |= tdi_mask; else clr_mask |= tdi_mask;

    if (set_mask) sio_hw->gpio_set = set_mask;
    if (clr_mask) sio_hw->gpio_clr = clr_mask;

    jtag_delay();
    sio_hw->gpio_set = tck_mask;
    jtag_delay();
    bool tdo = (sio_hw->gpio_in & tdo_mask) != 0;
    sio_hw->gpio_clr = tck_mask;
    jtag_delay();
    return tdo;
}

static inline void tick_payload_bit(bool tdi, bool last)
{
    // Specialised CFG_IN shifter: TDO is ignored and TMS remains low until the
    // final payload bit. Avoids most per-bit branching and all gpio_put() calls.
    if (tdi) sio_hw->gpio_set = tdi_mask;
    else     sio_hw->gpio_clr = tdi_mask;

    if (last) sio_hw->gpio_set = tms_mask;

    jtag_delay();
    sio_hw->gpio_set = tck_mask;
    jtag_delay();
    sio_hw->gpio_clr = tck_mask;
    jtag_delay();
}

static void idle_clocks(unsigned n)
{
    for (unsigned i = 0; i < n; i++) tick(false, false);
}

void jtag_gpio_init(void)
{
    gpio_init(M65_JTAG_TCK_PIN);
    gpio_init(M65_JTAG_TMS_PIN);
    gpio_init(M65_JTAG_TDI_PIN);
    gpio_init(M65_JTAG_TDO_PIN);
    gpio_init(M65_JTAG_HIJACK_PIN);

    gpio_set_dir(M65_JTAG_TCK_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TMS_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TDI_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TDO_PIN, GPIO_IN);

#if M65_JTAG_HIJACK_ACTIVE_HIGH
    gpio_put(M65_JTAG_HIJACK_PIN, 0);
#else
    gpio_put(M65_JTAG_HIJACK_PIN, 1);
#endif
    gpio_set_dir(M65_JTAG_HIJACK_PIN, GPIO_OUT);

#if M65_JTAG_DONE_PIN != 255
    gpio_init(M65_JTAG_DONE_PIN);
    gpio_set_dir(M65_JTAG_DONE_PIN, GPIO_IN);
#endif
#if M65_JTAG_INIT_B_PIN != 255
    gpio_init(M65_JTAG_INIT_B_PIN);
    gpio_set_dir(M65_JTAG_INIT_B_PIN, GPIO_IN);
#endif
}

void jtag_hijack_claim(void)
{
    // Avoid contention while the external switch changes over.
    gpio_set_dir(M65_JTAG_TCK_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TMS_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TDI_PIN, GPIO_IN);

#if M65_JTAG_HIJACK_ACTIVE_HIGH
    gpio_put(M65_JTAG_HIJACK_PIN, 1);
#else
    gpio_put(M65_JTAG_HIJACK_PIN, 0);
#endif
    sleep_us(10);

    gpio_put(M65_JTAG_TCK_PIN, 0);
    gpio_put(M65_JTAG_TMS_PIN, 1);
    gpio_put(M65_JTAG_TDI_PIN, 0);
    gpio_set_dir(M65_JTAG_TCK_PIN, GPIO_OUT);
    gpio_set_dir(M65_JTAG_TMS_PIN, GPIO_OUT);
    gpio_set_dir(M65_JTAG_TDI_PIN, GPIO_OUT);
    gpio_set_dir(M65_JTAG_TDO_PIN, GPIO_IN);
}

void jtag_hijack_release(void)
{
    gpio_put(M65_JTAG_TCK_PIN, 0);
    gpio_set_dir(M65_JTAG_TCK_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TMS_PIN, GPIO_IN);
    gpio_set_dir(M65_JTAG_TDI_PIN, GPIO_IN);
    sleep_us(10);
#if M65_JTAG_HIJACK_ACTIVE_HIGH
    gpio_put(M65_JTAG_HIJACK_PIN, 0);
#else
    gpio_put(M65_JTAG_HIJACK_PIN, 1);
#endif
}

static void tap_reset(void)
{
    for (unsigned i = 0; i < 8; i++) tick(true, false);
    tick(false, false); // Run-Test/Idle
}

static void shift_ir(uint32_t ir, unsigned ir_len)
{
    // Run-Test/Idle -> Select-DR -> Select-IR -> Capture-IR -> Shift-IR
    tick(true, false);
    tick(true, false);
    tick(false, false);
    tick(false, false);

    for (unsigned i = 0; i < ir_len; i++) {
        bool last = (i == ir_len - 1);
        tick(last, (ir >> i) & 1u); // IR is LSB-first.
    }

    // Exit1-IR -> Update-IR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);
}

static uint32_t shift_dr_read32(void)
{
    uint32_t v = 0;
    // Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR
    tick(true, false);
    tick(false, false);
    tick(false, false);

    for (unsigned i = 0; i < 32; i++) {
        bool last = (i == 31);
        if (tick(last, false)) v |= (1u << i); // DR readback is LSB-first.
    }

    // Exit1-DR -> Update-DR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);
    return v;
}

uint32_t jtag_read_idcode(void)
{
    last_err[0] = 0;
    tap_reset();
    shift_ir(XC7_IR_IDCODE, XC7_IR_LEN);
    return shift_dr_read32();
}

static bool stream_cfg_in_from_reader(uint32_t payload_length,
                                      jtag_stream_read_cb_t read_cb,
                                      void *read_ctx,
                                      const jtag_program_options_t *opts)
{
    static uint8_t buf[4096];
    uint32_t remaining = payload_length;
    uint32_t done = 0;

    if (!read_cb) {
        set_err("missing payload reader");
        return false;
    }

    // Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR
    tick(true, false);
    tick(false, false);
    tick(false, false);

    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        size_t got = 0;
        if (!read_cb(read_ctx, buf, want, &got) || got != want) {
            set_err("short read while streaming payload");
            return false;
        }

        for (size_t i = 0; i < got; i++) {
            uint8_t b = buf[i];
            bool final_byte = (remaining == 1 && i == got - 1);
            if (!final_byte) {
                // CFG_IN payload is MSB-first per byte, TMS=0 for all bits.
                tick_payload_bit((b & 0x80u) != 0, false);
                tick_payload_bit((b & 0x40u) != 0, false);
                tick_payload_bit((b & 0x20u) != 0, false);
                tick_payload_bit((b & 0x10u) != 0, false);
                tick_payload_bit((b & 0x08u) != 0, false);
                tick_payload_bit((b & 0x04u) != 0, false);
                tick_payload_bit((b & 0x02u) != 0, false);
                tick_payload_bit((b & 0x01u) != 0, false);
            } else {
                tick_payload_bit((b & 0x80u) != 0, false);
                tick_payload_bit((b & 0x40u) != 0, false);
                tick_payload_bit((b & 0x20u) != 0, false);
                tick_payload_bit((b & 0x10u) != 0, false);
                tick_payload_bit((b & 0x08u) != 0, false);
                tick_payload_bit((b & 0x04u) != 0, false);
                tick_payload_bit((b & 0x02u) != 0, false);
                tick_payload_bit((b & 0x01u) != 0, true);
            }
            remaining--;
            done++;
        }

        if (opts && opts->progress_cb && ((done & 0xffffu) == 0 || done == payload_length)) {
            opts->progress_cb(done, payload_length, opts->progress_ctx);
        }
    }

    // Exit1-DR -> Update-DR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);
    return true;
}

static bool idcode_match(uint32_t seen, uint32_t expected)
{
    if (expected == 0) return true;
    // Version field can vary; compare with upper nibble masked off, matching the
    // approach usually used for Xilinx IDCODE sanity checks.
    return (seen & 0x0fffffffu) == (expected & 0x0fffffffu);
}

bool jtag_program_stream(uint32_t payload_length,
                         uint32_t expected_idcode,
                         jtag_stream_read_cb_t read_cb,
                         void *read_ctx,
                         const jtag_program_options_t *opts)
{
    last_err[0] = 0;

    bool use_hijack = !opts || opts->use_hijack;
    bool release_after = !opts || opts->release_after;
    bool check_idcode = !opts || opts->check_idcode;

    if (payload_length == 0) {
        set_err("zero-length payload");
        return false;
    }

    if (use_hijack) jtag_hijack_claim();

    tap_reset();
    shift_ir(XC7_IR_IDCODE, XC7_IR_LEN);
    uint32_t id = shift_dr_read32();

    if (check_idcode && expected_idcode && !idcode_match(id, expected_idcode)) {
        snprintf(last_err, sizeof(last_err), "IDCODE mismatch: saw %08lx expected %08lx",
                 (unsigned long)id, (unsigned long)expected_idcode);
        if (release_after) jtag_hijack_release();
        return false;
    }

    shift_ir(XC7_IR_JPROGRAM, XC7_IR_LEN);
    idle_clocks(64);
    sleep_ms(10);

    shift_ir(XC7_IR_ISC_NOOP, XC7_IR_LEN);
    idle_clocks(64);
    sleep_ms(10);

    shift_ir(XC7_IR_CFG_IN, XC7_IR_LEN);
    if (!stream_cfg_in_from_reader(payload_length, read_cb, read_ctx, opts)) {
        if (release_after) jtag_hijack_release();
        return false;
    }

    shift_ir(XC7_IR_JSTART, XC7_IR_LEN);
    idle_clocks(128);

    shift_ir(XC7_IR_BYPASS, XC7_IR_LEN);
    tap_reset();

#if M65_JTAG_DONE_PIN != 255
    if (opts && opts->read_done_pin && gpio_get(M65_JTAG_DONE_PIN) == 0) {
        set_err("DONE pin is low after JSTART");
        if (release_after) jtag_hijack_release();
        return false;
    }
#endif

    if (release_after) jtag_hijack_release();
    return true;
}

typedef struct {
    core_file_t *cf;
} core_reader_ctx_t;

static bool core_reader(void *ctx, uint8_t *buf, size_t len, size_t *got)
{
    core_reader_ctx_t *cr = (core_reader_ctx_t *)ctx;
    if (!cr || !cr->cf) return false;
    return core_read_payload(cr->cf, buf, len, got);
}

bool jtag_program_core(core_file_t *cf, const jtag_program_options_t *opts)
{
    if (!cf) return false;
    if (!core_rewind_payload(cf)) {
        set_err("cannot seek to payload");
        return false;
    }

    core_reader_ctx_t ctx = { .cf = cf };
    return jtag_program_stream(cf->payload_length,
                               cf->expected_idcode,
                               core_reader,
                               &ctx,
                               opts);
}
