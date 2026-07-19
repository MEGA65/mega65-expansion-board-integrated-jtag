#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#ifndef M65_BOOTLOADER_SLOT0_OFFSET
#define M65_BOOTLOADER_SLOT0_OFFSET 0x10000u
#endif

#ifndef M65_BOOTLOADER_SLOT_SIZE
#define M65_BOOTLOADER_SLOT_SIZE 0xF0000u
#endif

#define XIP_BASE_ADDR 0x10000000u
#define SRAM_BASE_ADDR 0x20000000u
#define SRAM_END_ADDR  0x20042000u
#define PICO_BOOT2_LEN 0x100u
#define DIAG_UART_ID uart0
#define DIAG_UART_TX_PIN 0u
#define DIAG_UART_RX_PIN 1u

#ifndef M65_BOOT_DIAG
#define M65_BOOT_DIAG 0
#endif

#ifndef M65_BOOT_DIAG_UART_BAUD
#define M65_BOOT_DIAG_UART_BAUD 115200u
#endif

#ifndef M65_BOOT_DIAG_PIN
#ifdef PICO_DEFAULT_LED_PIN
#define M65_BOOT_DIAG_PIN PICO_DEFAULT_LED_PIN
#else
#define M65_BOOT_DIAG_PIN 255u
#endif
#endif

static inline uint32_t slot0_base(void)
{
    return XIP_BASE_ADDR + (uint32_t)M65_BOOTLOADER_SLOT0_OFFSET;
}

static bool in_range(uint32_t value, uint32_t base, uint32_t len)
{
    return value >= base && value < (base + len);
}

static bool valid_initial_sp(uint32_t value)
{
    if ((value & 0x3u) != 0) return false;
    return value >= SRAM_BASE_ADDR && value <= SRAM_END_ADDR;
}

static void diag_init(void)
{
#if M65_BOOT_DIAG
    uart_init(DIAG_UART_ID, M65_BOOT_DIAG_UART_BAUD);
    gpio_set_function(DIAG_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(DIAG_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(DIAG_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(DIAG_UART_ID, false, false);
    uart_set_fifo_enabled(DIAG_UART_ID, true);
#if M65_BOOT_DIAG_PIN != 255
    gpio_init(M65_BOOT_DIAG_PIN);
    gpio_set_dir(M65_BOOT_DIAG_PIN, GPIO_OUT);
    gpio_put(M65_BOOT_DIAG_PIN, 0);
#endif
#endif
}

static void diag_puts(const char *s)
{
#if M65_BOOT_DIAG
    uart_puts(DIAG_UART_ID, s);
    uart_tx_wait_blocking(DIAG_UART_ID);
#else
    (void)s;
#endif
}

static void diag_hex32(uint32_t value)
{
#if M65_BOOT_DIAG
    static const char hex[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc_raw(DIAG_UART_ID, hex[(value >> (unsigned)shift) & 0xfu]);
    }
#else
    (void)value;
#endif
}

static void diag_vectors(uint32_t initial_sp, uint32_t reset)
{
#if M65_BOOT_DIAG
    uart_puts(DIAG_UART_ID, "M65BOOT vectors sp=");
    diag_hex32(initial_sp);
    uart_puts(DIAG_UART_ID, " reset=");
    diag_hex32(reset);
    uart_puts(DIAG_UART_ID, "\r\n");
    uart_tx_wait_blocking(DIAG_UART_ID);
#else
    (void)initial_sp;
    (void)reset;
#endif
}

static void diag_stage(unsigned stage, const char *label)
{
#if M65_BOOT_DIAG
    uart_puts(DIAG_UART_ID, "M65BOOT ");
    uart_putc_raw(DIAG_UART_ID, (char)('0' + (stage % 10u)));
    uart_puts(DIAG_UART_ID, " ");
    uart_puts(DIAG_UART_ID, label);
    uart_puts(DIAG_UART_ID, "\r\n");
    uart_tx_wait_blocking(DIAG_UART_ID);
#if M65_BOOT_DIAG_PIN != 255
    for (unsigned i = 0; i < stage; i++) {
        gpio_put(M65_BOOT_DIAG_PIN, 1);
        busy_wait_ms(70);
        gpio_put(M65_BOOT_DIAG_PIN, 0);
        busy_wait_ms(90);
    }
    busy_wait_ms(180);
#endif
#else
    (void)stage;
    (void)label;
#endif
}

static bool slot_vectors_look_valid(uint32_t slot_base)
{
    const uint32_t *vectors = (const uint32_t *)(slot_base + PICO_BOOT2_LEN);
    uint32_t initial_sp = vectors[0];
    uint32_t reset = vectors[1];

    diag_vectors(initial_sp, reset);
    if (!valid_initial_sp(initial_sp)) return false;
    if ((reset & 0x1u) == 0) return false;

    uint32_t reset_addr = reset & ~0x1u;
    return in_range(reset_addr, slot_base, (uint32_t)M65_BOOTLOADER_SLOT_SIZE);
}

__attribute__((noreturn))
static void reboot_to_slot(uint32_t slot_base)
{
    const uint32_t *vectors = (const uint32_t *)(slot_base + PICO_BOOT2_LEN);
    uint32_t initial_sp = vectors[0];
    uint32_t reset = vectors[1];
    uint32_t reset_addr = reset & ~0x1u; // Address of Reset_Handler

    diag_stage(3, "reboot-to-app");

    // Clean up watchdog if we enabled it
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    // Disable SysTick
    *((volatile uint32_t *)0xe000e010) = 0;

    // Disable all NVIC interrupts and clear pending
    *((volatile uint32_t *)0xe000e180) = 0xFFFFFFFF; // ICER
    *((volatile uint32_t *)0xe000e280) = 0xFFFFFFFF; // ICPR

    // Set VTOR to the application vector table
    *((volatile uint32_t *)0xe000ed08) = (uint32_t)vectors;

    // Set SP and jump to the application's reset handler
    __asm volatile (
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r" (initial_sp), "r" (reset) // Use the original reset address with thumb bit
    );

    for (;;) {
        tight_loop_contents();
    }
}

int main(void)
{
    diag_init();
    diag_stage(1, "start");

    uint32_t slot = slot0_base();
    if (slot_vectors_look_valid(slot)) {
        diag_stage(2, "slot-valid");
        reboot_to_slot(slot);
    }

    diag_stage(9, "slot-invalid-bootsel");
    diag_puts("M65BOOT invalid app slot, entering BOOTSEL\r\n");
    reset_usb_boot(0, 0);
    while (true) {
        tight_loop_contents();
    }
}
