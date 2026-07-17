#include "bootsel_button.h"
#include "config.h"

#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef M65_BOOTSEL_BUTTON_POLL_MS
#define M65_BOOTSEL_BUTTON_POLL_MS 50u
#endif

static absolute_time_t bootsel_next_poll_at;
static bool bootsel_poll_scheduled;

static bool __no_inline_not_in_flash_func(bootsel_button_pressed_raw)(void)
{
    const uint cs_pin_index = 1;

    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; i++) {}

#ifdef __ARM_ARCH_6M__
    const uint32_t cs_bit = 1u << 1;
#else
    const uint32_t cs_bit = SIO_GPIO_HI_IN_QSPI_CSN_BITS;
#endif
    bool not_pressed = (sio_hw->gpio_hi_in & cs_bit) != 0;

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return !not_pressed;
}

void bootsel_button_poll(void)
{
#if M65_BOOTSEL_BUTTON_POLL_MS == 0
    return;
#else
    absolute_time_t now_abs = get_absolute_time();
    if (bootsel_poll_scheduled &&
        absolute_time_diff_us(now_abs, bootsel_next_poll_at) > 0) {
        return;
    }
    bootsel_next_poll_at = delayed_by_ms(now_abs, M65_BOOTSEL_BUTTON_POLL_MS);
    bootsel_poll_scheduled = true;

    if (!bootsel_button_pressed_raw()) {
        return;
    }

    sleep_ms(25);
    reset_usb_boot(0, 0);
#endif
}
