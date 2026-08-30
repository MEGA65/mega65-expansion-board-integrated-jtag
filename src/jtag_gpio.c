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
#define XC7_IR_CFG_OUT   0x04u

// Xilinx 7-series configuration packet helpers.
#define XC7_CONFIG_DUMMY   0xffffffffu
#define XC7_CONFIG_SYNC    0xaa995566u
#define XC7_TYPE1(op, reg, count) (0x20000000u | (((uint32_t)(op) & 3u) << 27) | (((uint32_t)(reg) & 0x3fffu) << 13) | ((uint32_t)(count) & 0x7ffu))
#define XC7_OP_NOP        0u
#define XC7_OP_READ       1u
#define XC7_OP_WRITE      2u
#define XC7_REG_STAT      7u
#define XC7_REG_BOOTSTS   22u

static char last_err[128];
static jtag_status_t last_status;
static bool last_status_valid;

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

static bool idcode_match(uint32_t seen, uint32_t expected);

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

    gpio_disable_pulls(M65_JTAG_TCK_PIN);
    gpio_disable_pulls(M65_JTAG_TMS_PIN);
    gpio_disable_pulls(M65_JTAG_TDI_PIN);
    gpio_disable_pulls(M65_JTAG_TDO_PIN);

#if M65_JTAG_HIJACK_ACTIVE_HIGH
    gpio_put(M65_JTAG_HIJACK_PIN, 0);
#else
    gpio_put(M65_JTAG_HIJACK_PIN, 1);
#endif
    gpio_set_dir(M65_JTAG_HIJACK_PIN, GPIO_OUT);

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

static uint32_t shift_dr_read32_msb(void)
{
    uint32_t v = 0;
    // Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR
    tick(true, false);
    tick(false, false);
    tick(false, false);

    for (unsigned i = 0; i < 32; i++) {
        bool last = (i == 31);
        v <<= 1;
        if (tick(last, false)) v |= 1u; // CFG_OUT words are config-data ordered.
    }

    // Exit1-DR -> Update-DR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);
    return v;
}

static void shift_dr_write_word_msb(uint32_t w, bool last_word)
{
    for (int b = 31; b >= 0; b--) {
        bool last = last_word && (b == 0);
        tick_payload_bit(((w >> b) & 1u) != 0, last);
    }
}

static void shift_dr_write_words_msb(const uint32_t *words, size_t count)
{
    // Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR
    tick(true, false);
    tick(false, false);
    tick(false, false);

    for (size_t i = 0; i < count; i++) {
        shift_dr_write_word_msb(words[i], i == count - 1);
    }

    // Exit1-DR -> Update-DR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);
}

static bool read_config_reg(uint32_t reg, uint32_t *value)
{
    if (!value) return false;

    // Based on the 7-series JTAG configuration-register read procedure:
    // feed a small read request through CFG_IN, then shift the register value
    // out through CFG_OUT.  Single-device chain only.
    const uint32_t req[] = {
        XC7_CONFIG_DUMMY,
        XC7_CONFIG_SYNC,
        XC7_TYPE1(XC7_OP_NOP, 0, 0),
        XC7_TYPE1(XC7_OP_READ, reg, 1),
        XC7_TYPE1(XC7_OP_NOP, 0, 0),
        XC7_TYPE1(XC7_OP_NOP, 0, 0),
    };

    shift_ir(XC7_IR_CFG_IN, XC7_IR_LEN);
    shift_dr_write_words_msb(req, sizeof(req) / sizeof(req[0]));
    idle_clocks(16);

    shift_ir(XC7_IR_CFG_OUT, XC7_IR_LEN);
    *value = shift_dr_read32_msb();
    idle_clocks(16);

    shift_ir(XC7_IR_BYPASS, XC7_IR_LEN);
    idle_clocks(16);
    return true;
}

uint32_t jtag_read_idcode(void)
{
    last_err[0] = 0;
    tap_reset();
    shift_ir(XC7_IR_IDCODE, XC7_IR_LEN);
    return shift_dr_read32();
}

bool jtag_read_xilinx_status(jtag_status_t *st)
{
    if (!st) return false;
    memset(st, 0, sizeof(*st));
    last_err[0] = 0;

    tap_reset();
    shift_ir(XC7_IR_IDCODE, XC7_IR_LEN);
    st->idcode = shift_dr_read32();

    st->bootsts_valid = read_config_reg(XC7_REG_BOOTSTS, &st->bootsts);
    st->stat_valid = read_config_reg(XC7_REG_STAT, &st->stat);

    shift_ir(XC7_IR_BYPASS, XC7_IR_LEN);
    st->bypass = shift_dr_read32();
    st->bypass_valid = true;

    tap_reset();
    return st->bootsts_valid && st->stat_valid;
}

bool jtag_get_last_status(jtag_status_t *st)
{
    if (!st || !last_status_valid) return false;
    *st = last_status;
    return true;
}

static void jtag_finish_closeout(void)
{
    // Exit1-DR -> Update-DR -> Run-Test/Idle
    tick(true, false);
    tick(false, false);

    // Start-up close-out. 128 clocks was enough for the normal MEGA65 core,
    // but some cores appear to fall through into configuration fallback/QSPI
    // boot unless we keep the TAP in Run-Test/Idle for longer after JSTART.
    // These counts are config.h tunables so we can trim them later.
    shift_ir(XC7_IR_JSTART, XC7_IR_LEN);
    idle_clocks(M65_JTAG_POST_JSTART_IDLE_CLOCKS);

    // Put the device into an innocuous instruction and provide more idle clocks
    // before selecting BYPASS/resetting the TAP. This is deliberately conservative
    // and closer to what a vendor tool does than a minimal byte-shifter.
    shift_ir(XC7_IR_ISC_NOOP, XC7_IR_LEN);
    idle_clocks(M65_JTAG_POST_ISC_NOOP_IDLE_CLOCKS);

    shift_ir(XC7_IR_BYPASS, XC7_IR_LEN);
    idle_clocks(M65_JTAG_POST_BYPASS_IDLE_CLOCKS);

    tap_reset();
    idle_clocks(M65_JTAG_POST_TAP_RESET_IDLE_CLOCKS);

    // Capture Xilinx config status before releasing the hijack switch. This is
    // diagnostic only for now: shifting all bytes is not the same as proving
    // CRC/DONE/EOS/startup state is good, so expose BOOTSTS/STAT/BYPASS to the
    // command layer after OK/ERR.
    last_status_valid = jtag_read_xilinx_status(&last_status);
}

bool jtag_program_writer_begin(jtag_stream_writer_t *wr,
                               uint32_t payload_length,
                               uint32_t expected_idcode,
                               const jtag_program_options_t *opts)
{
    if (!wr) return false;
    memset(wr, 0, sizeof *wr);
    last_err[0] = 0;
    last_status_valid = false;

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

    // Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR. The writer stays
    // in Shift-DR until the final payload bit is written with TMS=1.
    tick(true, false);
    tick(false, false);
    tick(false, false);

    wr->payload_length = payload_length;
    wr->expected_idcode = expected_idcode;
    wr->release_after = release_after;
    wr->progress_cb = opts ? opts->progress_cb : NULL;
    wr->progress_ctx = opts ? opts->progress_ctx : NULL;
    wr->active = true;
    return true;
}

bool jtag_program_writer_write(jtag_stream_writer_t *wr, const uint8_t *buf, size_t len)
{
    if (!wr || !wr->active || (!buf && len)) {
        set_err("invalid JTAG writer");
        return false;
    }
    if (wr->done + len > wr->payload_length) {
        set_err("payload longer than declared length");
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        bool final_byte = (wr->done + 1u) == wr->payload_length;
        tick_payload_bit((b & 0x80u) != 0, false);
        tick_payload_bit((b & 0x40u) != 0, false);
        tick_payload_bit((b & 0x20u) != 0, false);
        tick_payload_bit((b & 0x10u) != 0, false);
        tick_payload_bit((b & 0x08u) != 0, false);
        tick_payload_bit((b & 0x04u) != 0, false);
        tick_payload_bit((b & 0x02u) != 0, false);
        tick_payload_bit((b & 0x01u) != 0, final_byte);
        wr->done++;

        if (wr->progress_cb && ((wr->done & 0xffffu) == 0 || wr->done == wr->payload_length)) {
            wr->progress_cb(wr->done, wr->payload_length, wr->progress_ctx);
        }
    }
    return true;
}

bool jtag_program_writer_finish(jtag_stream_writer_t *wr)
{
    if (!wr || !wr->active) {
        set_err("invalid JTAG writer");
        return false;
    }
    if (wr->done != wr->payload_length) {
        set_err("short payload");
        jtag_program_writer_abort(wr);
        return false;
    }

    jtag_finish_closeout();
    if (wr->release_after) jtag_hijack_release();
    wr->active = false;
    return true;
}

void jtag_program_writer_abort(jtag_stream_writer_t *wr)
{
    if (!wr || !wr->active) return;
    tap_reset();
    if (wr->release_after) jtag_hijack_release();
    wr->active = false;
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
    static uint8_t buf[4096];
    jtag_stream_writer_t wr;

    if (!read_cb) {
        set_err("missing payload reader");
        return false;
    }

    if (!jtag_program_writer_begin(&wr, payload_length, expected_idcode, opts)) return false;

    uint32_t remaining = payload_length;
    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        size_t got = 0;
        if (!read_cb(read_ctx, buf, want, &got) || got != want) {
            set_err("short read while streaming payload");
            jtag_program_writer_abort(&wr);
            return false;
        }
        if (!jtag_program_writer_write(&wr, buf, got)) {
            jtag_program_writer_abort(&wr);
            return false;
        }
        remaining -= (uint32_t)got;
    }

    return jtag_program_writer_finish(&wr);
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
