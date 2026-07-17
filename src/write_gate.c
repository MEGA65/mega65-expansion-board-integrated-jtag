#include "write_gate.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#ifndef M65_WRITE_ENABLE_PIN
#define M65_WRITE_ENABLE_PIN 255u
#endif
#ifndef M65_WRITE_ENABLE_ACTIVE_LOW
#define M65_WRITE_ENABLE_ACTIVE_LOW 1
#endif
#ifndef M65_WRITE_ENABLE_TIMEOUT_MS
#define M65_WRITE_ENABLE_TIMEOUT_MS 120000u
#endif

static uint64_t write_auth_until_us = 0;

bool write_gate_physical_asserted(void)
{
#if M65_WRITE_ENABLE_PIN == 255
    return false;
#else
    bool level = gpio_get(M65_WRITE_ENABLE_PIN);
#if M65_WRITE_ENABLE_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
#endif
}

void write_gate_refresh_from_pin(void)
{
    if (write_gate_physical_asserted()) {
        write_auth_until_us = time_us_64() + ((uint64_t)M65_WRITE_ENABLE_TIMEOUT_MS * 1000ull);
    }
}

bool write_gate_active(void)
{
    write_gate_refresh_from_pin();
    uint64_t now = time_us_64();
    return write_auth_until_us != 0 && now < write_auth_until_us;
}

uint32_t write_gate_remaining_ms(void)
{
    write_gate_refresh_from_pin();
    uint64_t now = time_us_64();
    if (write_auth_until_us == 0 || now >= write_auth_until_us) return 0;
    return (uint32_t)((write_auth_until_us - now) / 1000ull);
}

void write_gate_init(void)
{
#if M65_WRITE_ENABLE_PIN != 255
    gpio_init(M65_WRITE_ENABLE_PIN);
    gpio_set_dir(M65_WRITE_ENABLE_PIN, GPIO_IN);
#if M65_WRITE_ENABLE_ACTIVE_LOW
    gpio_pull_up(M65_WRITE_ENABLE_PIN);
#else
    gpio_pull_down(M65_WRITE_ENABLE_PIN);
#endif
#endif
    write_auth_until_us = 0;
}
