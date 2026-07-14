// Local SD SPI transport for carlk3/no-OS-FatFS-SD-SPI-RPi-Pico.
// The firmware carries two board layouts at runtime:
//   - corrected hardware SPI: GP2=SCK, GP3=MOSI, GP4=MISO, GP5=CS
//   - first schematic bit-bang: GP2=CS, GP3=MOSI, GP4=SCK, GP5=MISO

#include "config.h"
#include "storage.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/structs/sio.h"
#include "pico/mutex.h"
#include "pico/stdlib.h"

#include "sd_card.h"
#include "sd_spi.h"
#include "spi.h"

#ifndef M65_SD_AUTO_FALLBACK_MODE
#define M65_SD_AUTO_FALLBACK_MODE M65_SD_MODE_SCHEMATIC_BITBANG
#endif
#ifndef M65_SD_BITBANG_LOW_HALF_PERIOD_US
#define M65_SD_BITBANG_LOW_HALF_PERIOD_US 2u
#endif
#ifndef M65_SD_BITBANG_FAST_DELAY_LOOPS
#define M65_SD_BITBANG_FAST_DELAY_LOOPS 0u
#endif

typedef enum {
    SD_REQUEST_AUTO = 0,
    SD_REQUEST_HW_SPI,
    SD_REQUEST_SCHEMATIC_BITBANG,
} sd_request_t;

typedef enum {
    SD_ACTIVE_NONE = 0,
    SD_ACTIVE_HW_SPI,
    SD_ACTIVE_SCHEMATIC_BITBANG,
} sd_active_t;

#if M65_SD_MODE == M65_SD_MODE_HW_SPI
static sd_request_t requested_mode = SD_REQUEST_HW_SPI;
#elif M65_SD_MODE == M65_SD_MODE_SCHEMATIC_BITBANG
static sd_request_t requested_mode = SD_REQUEST_SCHEMATIC_BITBANG;
#else
static sd_request_t requested_mode = SD_REQUEST_AUTO;
#endif

static sd_active_t active_mode = SD_ACTIVE_NONE;
static bool transport_locked = false;
static bool used_auto_fallback = false;
static uint32_t bitbang_half_period_us = M65_SD_BITBANG_LOW_HALF_PERIOD_US;

#define HW_SPI_DMA_THRESHOLD 32u

static inline uint32_t gpio_mask_u32(uint gpio)
{
    return 1u << gpio;
}

static const char *request_name(sd_request_t mode)
{
    switch (mode) {
    case SD_REQUEST_AUTO: return "auto";
    case SD_REQUEST_HW_SPI: return "hw";
    case SD_REQUEST_SCHEMATIC_BITBANG: return "soft";
    default: return "?";
    }
}

static const char *active_name(sd_active_t mode)
{
    switch (mode) {
    case SD_ACTIVE_HW_SPI: return "hw-spi";
    case SD_ACTIVE_SCHEMATIC_BITBANG: return "schematic-bitbang";
    case SD_ACTIVE_NONE: return "none";
    default: return "?";
    }
}

static sd_active_t fallback_active_mode(void)
{
#if M65_SD_AUTO_FALLBACK_MODE == M65_SD_MODE_HW_SPI
    return SD_ACTIVE_HW_SPI;
#else
    return SD_ACTIVE_SCHEMATIC_BITBANG;
#endif
}

static bool arg_equals(const char *arg, const char *word)
{
    while (*arg && isspace((unsigned char)*arg)) {
        ++arg;
    }

    while (*arg && *word) {
        if (tolower((unsigned char)*arg) != *word) {
            return false;
        }
        ++arg;
        ++word;
    }

    while (*arg && isspace((unsigned char)*arg)) {
        ++arg;
    }

    return *arg == 0 && *word == 0;
}

static void deselect_candidate_cs_pins(void)
{
    gpio_init(M65_SD_SOFT_CS_PIN);
    gpio_put(M65_SD_SOFT_CS_PIN, 1);
    gpio_set_dir(M65_SD_SOFT_CS_PIN, GPIO_OUT);
    gpio_put(M65_SD_SOFT_CS_PIN, 1);

    gpio_init(M65_SD_HW_CS_PIN);
    gpio_put(M65_SD_HW_CS_PIN, 1);
    gpio_set_dir(M65_SD_HW_CS_PIN, GPIO_OUT);
    gpio_put(M65_SD_HW_CS_PIN, 1);
}

static inline void bitbang_delay(void)
{
    if (bitbang_half_period_us) {
        busy_wait_us_32(bitbang_half_period_us);
        return;
    }

    for (uint32_t i = 0; i < M65_SD_BITBANG_FAST_DELAY_LOOPS; ++i) {
        tight_loop_contents();
    }
}

static uint8_t bitbang_transfer_pins(uint sck_gpio, uint mosi_gpio, uint miso_gpio, uint8_t tx)
{
    const uint32_t sck_mask = gpio_mask_u32(sck_gpio);
    const uint32_t mosi_mask = gpio_mask_u32(mosi_gpio);
    const uint32_t miso_mask = gpio_mask_u32(miso_gpio);
    uint8_t rx = 0;

    for (unsigned bit = 0; bit < 8; ++bit) {
        if (tx & 0x80u) {
            sio_hw->gpio_set = mosi_mask;
        } else {
            sio_hw->gpio_clr = mosi_mask;
        }

        bitbang_delay();
        sio_hw->gpio_set = sck_mask;
        bitbang_delay();

        rx = (uint8_t)((rx << 1) | ((sio_hw->gpio_in & miso_mask) ? 1u : 0u));
        sio_hw->gpio_clr = sck_mask;
        bitbang_delay();
        tx <<= 1;
    }

    return rx;
}

static uint8_t bitbang_transfer_byte(spi_t *spi_p, uint8_t tx)
{
    return bitbang_transfer_pins(spi_p->sck_gpio, spi_p->mosi_gpio, spi_p->miso_gpio, tx);
}

typedef uint8_t (*probe_xfer_fn)(uint8_t tx, void *ctx);

static bool probe_cmd0(uint cs_gpio, probe_xfer_fn xfer, void *ctx)
{
    static const uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};

    for (unsigned attempt = 0; attempt < M65_SD_PROBE_RETRIES; ++attempt) {
        gpio_put(cs_gpio, 1);
        for (unsigned i = 0; i < 10; ++i) {
            (void)xfer(SPI_FILL_CHAR, ctx);
        }

        gpio_put(cs_gpio, 0);
        for (unsigned i = 0; i < sizeof cmd0; ++i) {
            (void)xfer(cmd0[i], ctx);
        }

        uint8_t response = SPI_FILL_CHAR;
        for (unsigned i = 0; i < 32; ++i) {
            response = xfer(SPI_FILL_CHAR, ctx);
            if ((response & 0x80u) == 0) {
                break;
            }
        }

        gpio_put(cs_gpio, 1);
        (void)xfer(SPI_FILL_CHAR, ctx);
        if (response == 0x01u) {
            return true;
        }

        if (M65_SD_PROBE_RETRY_DELAY_MS) {
            sleep_ms(M65_SD_PROBE_RETRY_DELAY_MS);
        }
    }

    return false;
}

typedef struct {
    uint sck_gpio;
    uint mosi_gpio;
    uint miso_gpio;
} bitbang_probe_t;

static uint8_t probe_bitbang_xfer(uint8_t tx, void *ctx)
{
    const bitbang_probe_t *pins = (const bitbang_probe_t *)ctx;
    return bitbang_transfer_pins(pins->sck_gpio, pins->mosi_gpio, pins->miso_gpio, tx);
}

static bool probe_schematic_bitbang(void)
{
    const bitbang_probe_t pins = {
        .sck_gpio = M65_SD_SOFT_SCK_PIN,
        .mosi_gpio = M65_SD_SOFT_MOSI_PIN,
        .miso_gpio = M65_SD_SOFT_MISO_PIN,
    };

    deselect_candidate_cs_pins();
    bitbang_half_period_us = M65_SD_BITBANG_LOW_HALF_PERIOD_US;

    gpio_init(M65_SD_SOFT_SCK_PIN);
    gpio_put(M65_SD_SOFT_SCK_PIN, 0);
    gpio_set_dir(M65_SD_SOFT_SCK_PIN, GPIO_OUT);

    gpio_init(M65_SD_SOFT_MOSI_PIN);
    gpio_put(M65_SD_SOFT_MOSI_PIN, 1);
    gpio_set_dir(M65_SD_SOFT_MOSI_PIN, GPIO_OUT);

    gpio_init(M65_SD_SOFT_MISO_PIN);
    gpio_set_dir(M65_SD_SOFT_MISO_PIN, GPIO_IN);
    gpio_pull_up(M65_SD_SOFT_MISO_PIN);

    gpio_put(M65_SD_SOFT_CS_PIN, 1);
    gpio_set_dir(M65_SD_SOFT_CS_PIN, GPIO_OUT);
    return probe_cmd0(M65_SD_SOFT_CS_PIN, probe_bitbang_xfer, (void *)&pins);
}

static uint8_t probe_hw_xfer(uint8_t tx, void *ctx)
{
    (void)ctx;
    uint8_t rx = SPI_FILL_CHAR;
    (void)spi_write_read_blocking(M65_SD_HW_SPI_ID, &tx, &rx, 1);
    return rx;
}

static bool probe_hw_spi(void)
{
    deselect_candidate_cs_pins();

    gpio_put(M65_SD_HW_CS_PIN, 1);
    gpio_set_dir(M65_SD_HW_CS_PIN, GPIO_OUT);

    spi_init(M65_SD_HW_SPI_ID, 400u * 1000u);
    spi_set_format(M65_SD_HW_SPI_ID, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(M65_SD_HW_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(M65_SD_HW_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(M65_SD_HW_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(M65_SD_HW_MISO_PIN);

    return probe_cmd0(M65_SD_HW_CS_PIN, probe_hw_xfer, NULL);
}

void storage_sd_probe(void)
{
    if (transport_locked) {
        return;
    }

    used_auto_fallback = false;
    active_mode = SD_ACTIVE_NONE;

    if (requested_mode == SD_REQUEST_HW_SPI) {
        active_mode = SD_ACTIVE_HW_SPI;
        return;
    }

    if (requested_mode == SD_REQUEST_SCHEMATIC_BITBANG) {
        active_mode = SD_ACTIVE_SCHEMATIC_BITBANG;
        return;
    }

    if (probe_schematic_bitbang()) {
        active_mode = SD_ACTIVE_SCHEMATIC_BITBANG;
        return;
    }

    if (probe_hw_spi()) {
        active_mode = SD_ACTIVE_HW_SPI;
        return;
    }
}

bool storage_sd_may_mount(void)
{
    if (transport_locked) {
        return true;
    }

    if (requested_mode != SD_REQUEST_AUTO) {
        return true;
    }

    storage_sd_probe();
    return active_mode != SD_ACTIVE_NONE;
}

bool storage_sd_set_transport(const char *name)
{
    if (transport_locked) {
        return false;
    }

    if (arg_equals(name, "auto")) {
        requested_mode = SD_REQUEST_AUTO;
    } else if (arg_equals(name, "hw") || arg_equals(name, "hardware") ||
               arg_equals(name, "hardware-spi")) {
        requested_mode = SD_REQUEST_HW_SPI;
    } else if (arg_equals(name, "soft") || arg_equals(name, "software") ||
               arg_equals(name, "bitbang") || arg_equals(name, "schematic")) {
        requested_mode = SD_REQUEST_SCHEMATIC_BITBANG;
    } else {
        return false;
    }

    active_mode = SD_ACTIVE_NONE;
    used_auto_fallback = false;
    return true;
}

bool storage_sd_transport_locked(void)
{
    return transport_locked;
}

const char *storage_sd_transport_name(void)
{
    static char name[96];
    snprintf(name, sizeof name, "request=%s,active=%s%s,locked=%lu",
             request_name(requested_mode),
             active_name(active_mode),
             used_auto_fallback ? "-fallback" : "",
             (unsigned long)(transport_locked ? 1u : 0u));
    return name;
}

void m65_sd_runtime_configure(spi_t *spi_p, sd_card_t *pSD)
{
    if (transport_locked) {
        return;
    }

    if (active_mode == SD_ACTIVE_NONE) {
        storage_sd_probe();
    }

    if (active_mode == SD_ACTIVE_NONE) {
        active_mode = fallback_active_mode();
        used_auto_fallback = true;
    }

    if (active_mode == SD_ACTIVE_HW_SPI) {
        spi_p->hw_inst = M65_SD_HW_SPI_ID;
        spi_p->sck_gpio = M65_SD_HW_SCK_PIN;
        spi_p->mosi_gpio = M65_SD_HW_MOSI_PIN;
        spi_p->miso_gpio = M65_SD_HW_MISO_PIN;
        pSD->ss_gpio = M65_SD_HW_CS_PIN;
    } else {
        spi_p->hw_inst = M65_SD_HW_SPI_ID;
        spi_p->sck_gpio = M65_SD_SOFT_SCK_PIN;
        spi_p->mosi_gpio = M65_SD_SOFT_MOSI_PIN;
        spi_p->miso_gpio = M65_SD_SOFT_MISO_PIN;
        pSD->ss_gpio = M65_SD_SOFT_CS_PIN;
    }
}

void spi_lock(spi_t *spi_p)
{
    assert(spi_p);
    if (!mutex_is_initialized(&spi_p->mutex)) {
        mutex_init(&spi_p->mutex);
    }
    mutex_enter_blocking(&spi_p->mutex);
}

void spi_unlock(spi_t *spi_p)
{
    assert(spi_p);
    assert(mutex_is_initialized(&spi_p->mutex));
    mutex_exit(&spi_p->mutex);
}

void set_spi_dma_irq_channel(bool useChannel1, bool shared)
{
    (void)useChannel1;
    (void)shared;
}

bool my_spi_init(spi_t *spi_p)
{
    if (!spi_p) {
        return false;
    }

    if (!mutex_is_initialized(&spi_p->mutex)) {
        mutex_init(&spi_p->mutex);
    }

    spi_lock(spi_p);
    if (spi_p->initialized) {
        spi_unlock(spi_p);
        return true;
    }

    if (!spi_p->baud_rate) {
        spi_p->baud_rate = 10u * 1000u * 1000u;
    }

    transport_locked = true;

    if (active_mode == SD_ACTIVE_HW_SPI) {
        spi_init(spi_p->hw_inst, 400u * 1000u);
        spi_set_format(spi_p->hw_inst, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(spi_p->miso_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi_p->mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi_p->sck_gpio, GPIO_FUNC_SPI);
        gpio_pull_up(spi_p->miso_gpio);

        spi_p->tx_dma = dma_claim_unused_channel(true);
        spi_p->rx_dma = dma_claim_unused_channel(true);

        spi_p->tx_dma_cfg = dma_channel_get_default_config(spi_p->tx_dma);
        channel_config_set_transfer_data_size(&spi_p->tx_dma_cfg, DMA_SIZE_8);
        channel_config_set_dreq(&spi_p->tx_dma_cfg,
                                spi_get_index(spi_p->hw_inst) ? DREQ_SPI1_TX : DREQ_SPI0_TX);
        channel_config_set_write_increment(&spi_p->tx_dma_cfg, false);

        spi_p->rx_dma_cfg = dma_channel_get_default_config(spi_p->rx_dma);
        channel_config_set_transfer_data_size(&spi_p->rx_dma_cfg, DMA_SIZE_8);
        channel_config_set_dreq(&spi_p->rx_dma_cfg,
                                spi_get_index(spi_p->hw_inst) ? DREQ_SPI1_RX : DREQ_SPI0_RX);
        channel_config_set_read_increment(&spi_p->rx_dma_cfg, false);
    } else {
        gpio_init(spi_p->sck_gpio);
        gpio_init(spi_p->mosi_gpio);
        gpio_init(spi_p->miso_gpio);

        gpio_put(spi_p->sck_gpio, 0);
        gpio_put(spi_p->mosi_gpio, 1);
        gpio_set_dir(spi_p->sck_gpio, GPIO_OUT);
        gpio_set_dir(spi_p->mosi_gpio, GPIO_OUT);
        gpio_set_dir(spi_p->miso_gpio, GPIO_IN);
        gpio_pull_up(spi_p->miso_gpio);
        bitbang_half_period_us = M65_SD_BITBANG_LOW_HALF_PERIOD_US;
    }

    if (spi_p->set_drive_strength) {
        gpio_set_drive_strength(spi_p->mosi_gpio, spi_p->mosi_gpio_drive_strength);
        gpio_set_drive_strength(spi_p->sck_gpio, spi_p->sck_gpio_drive_strength);
    }

    spi_p->initialized = true;
    spi_unlock(spi_p);
    return true;
}

static bool hw_spi_dma_transfer(spi_t *spi_p, const uint8_t *tx, uint8_t *rx, size_t length)
{
    static const uint8_t dummy_tx = SPI_FILL_CHAR;
    static uint8_t dummy_rx;

    dma_channel_config tx_cfg = spi_p->tx_dma_cfg;
    dma_channel_config rx_cfg = spi_p->rx_dma_cfg;

    if (tx) {
        channel_config_set_read_increment(&tx_cfg, true);
    } else {
        tx = &dummy_tx;
        channel_config_set_read_increment(&tx_cfg, false);
    }

    if (rx) {
        channel_config_set_write_increment(&rx_cfg, true);
    } else {
        rx = &dummy_rx;
        channel_config_set_write_increment(&rx_cfg, false);
    }

    dma_channel_configure(spi_p->tx_dma, &tx_cfg,
                          &spi_get_hw(spi_p->hw_inst)->dr,
                          tx,
                          length,
                          false);
    dma_channel_configure(spi_p->rx_dma, &rx_cfg,
                          rx,
                          &spi_get_hw(spi_p->hw_inst)->dr,
                          length,
                          false);

    dma_start_channel_mask((1u << spi_p->tx_dma) | (1u << spi_p->rx_dma));
    dma_channel_wait_for_finish_blocking(spi_p->tx_dma);
    dma_channel_wait_for_finish_blocking(spi_p->rx_dma);
    return true;
}

bool spi_transfer(spi_t *spi_p, const uint8_t *tx, uint8_t *rx, size_t length)
{
    assert(spi_p);
    assert(tx || rx);

    if (length == 0) {
        return true;
    }

    if (active_mode == SD_ACTIVE_HW_SPI) {
        if (length >= HW_SPI_DMA_THRESHOLD) {
            return hw_spi_dma_transfer(spi_p, tx, rx, length);
        }

        int count;
        if (tx && rx) {
            count = spi_write_read_blocking(spi_p->hw_inst, tx, rx, length);
        } else if (tx) {
            count = spi_write_blocking(spi_p->hw_inst, tx, length);
        } else {
            count = spi_read_blocking(spi_p->hw_inst, SPI_FILL_CHAR, rx, length);
        }
        return count == (int)length;
    }

    for (size_t i = 0; i < length; ++i) {
        const uint8_t out = tx ? tx[i] : SPI_FILL_CHAR;
        uint8_t in = SPI_FILL_CHAR;

        in = bitbang_transfer_byte(spi_p, out);

        if (rx) {
            rx[i] = in;
        }
    }

    return true;
}

static void sd_spi_select(sd_card_t *pSD)
{
    gpio_put(pSD->ss_gpio, 0);
    uint8_t fill = SPI_FILL_CHAR;
    (void)spi_transfer(pSD->spi, &fill, NULL, 1);
    LED_ON();
}

static void sd_spi_deselect(sd_card_t *pSD)
{
    gpio_put(pSD->ss_gpio, 1);
    LED_OFF();

    uint8_t fill = SPI_FILL_CHAR;
    (void)spi_transfer(pSD->spi, &fill, NULL, 1);
}

void sd_spi_deselect_pulse(sd_card_t *pSD)
{
    sd_spi_deselect(pSD);
    sd_spi_select(pSD);
}

void sd_spi_acquire(sd_card_t *pSD)
{
    spi_lock(pSD->spi);
    sd_spi_select(pSD);
}

void sd_spi_release(sd_card_t *pSD)
{
    sd_spi_deselect(pSD);
    spi_unlock(pSD->spi);
}

bool sd_spi_transfer(sd_card_t *pSD, const uint8_t *tx, uint8_t *rx, size_t length)
{
    return spi_transfer(pSD->spi, tx, rx, length);
}

uint8_t sd_spi_write(sd_card_t *pSD, const uint8_t value)
{
    uint8_t received = SPI_FILL_CHAR;
    bool success = spi_transfer(pSD->spi, &value, &received, 1);
    assert(success);
    return received;
}

void sd_spi_go_low_frequency(sd_card_t *pSD)
{
    if (active_mode == SD_ACTIVE_HW_SPI) {
        (void)spi_set_baudrate(pSD->spi->hw_inst, 400u * 1000u);
    } else {
        bitbang_half_period_us = M65_SD_BITBANG_LOW_HALF_PERIOD_US;
    }
}

void sd_spi_go_high_frequency(sd_card_t *pSD)
{
    if (active_mode == SD_ACTIVE_HW_SPI) {
        (void)spi_set_baudrate(pSD->spi->hw_inst, pSD->spi->baud_rate);
    } else {
        (void)pSD;
        bitbang_half_period_us = 0;
    }
}

void sd_spi_send_initializing_sequence(sd_card_t *pSD)
{
    bool old_ss = gpio_get(pSD->ss_gpio);
    gpio_put(pSD->ss_gpio, 1);

    uint8_t ones[10];
    memset(ones, SPI_FILL_CHAR, sizeof ones);
    absolute_time_t timeout_time = make_timeout_time_ms(1);
    do {
        (void)sd_spi_transfer(pSD, ones, NULL, sizeof ones);
    } while (0 < absolute_time_diff_us(get_absolute_time(), timeout_time));

    gpio_put(pSD->ss_gpio, old_ss);
}

void sd_spi_init_pl022(sd_card_t *pSD)
{
    if (active_mode == SD_ACTIVE_HW_SPI) {
        gpio_set_function(pSD->ss_gpio, GPIO_FUNC_SPI);
    }
}
