// Hardware configuration glue for carlk3/no-OS-FatFS-SD-SPI-RPi-Pico.
// This file is only compiled when M65_USE_FATFS=1.

#include "config.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "sd_driver/hw_config.h"
#include "sd_driver/spi.h"
#include "sd_driver/sd_card.h"

void m65_sd_runtime_configure(spi_t *spi_p, sd_card_t *pSD);

static spi_t spis[] = {
    {
        .hw_inst = M65_SD_SPI_ID,
        .miso_gpio = M65_SD_MISO_PIN,
        .mosi_gpio = M65_SD_MOSI_PIN,
        .sck_gpio = M65_SD_SCK_PIN,
        .baud_rate = M65_SD_SPI_BAUD,
        .set_drive_strength = false,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    }
};

static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",
        .spi = &spis[0],
        .ss_gpio = M65_SD_CS_PIN,
        .use_card_detect = false,
        .card_detect_gpio = 0,
        .card_detected_true = 1,
        .set_drive_strength = false,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    }
};

static void configure_runtime_layout(void)
{
    m65_sd_runtime_configure(&spis[0], &sd_cards[0]);
}

size_t spi_get_num(void)
{
    return sizeof spis / sizeof spis[0];
}

spi_t *spi_get_by_num(size_t num)
{
    if (num >= spi_get_num()) return NULL;
    if (num == 0) configure_runtime_layout();
    return &spis[num];
}

size_t sd_get_num(void)
{
    return sizeof sd_cards / sizeof sd_cards[0];
}

sd_card_t *sd_get_by_num(size_t num)
{
    if (num >= sd_get_num()) return NULL;
    if (num == 0) configure_runtime_layout();
    return &sd_cards[num];
}
