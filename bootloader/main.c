#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/m0plus.h"
#include "hardware/structs/scb.h"
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

static inline uint32_t slot0_base(void)
{
    return XIP_BASE_ADDR + (uint32_t)M65_BOOTLOADER_SLOT0_OFFSET;
}

static bool in_range(uint32_t value, uint32_t base, uint32_t len)
{
    return value >= base && value < (base + len);
}

static bool slot_vectors_look_valid(uint32_t slot_base)
{
    const uint32_t *vectors = (const uint32_t *)(slot_base + PICO_BOOT2_LEN);
    uint32_t initial_sp = vectors[0];
    uint32_t reset = vectors[1];

    if ((initial_sp & 0x3u) != 0) return false;
    if (!in_range(initial_sp, SRAM_BASE_ADDR, SRAM_END_ADDR)) return false;
    if ((reset & 0x1u) == 0) return false;

    uint32_t reset_addr = reset & ~0x1u;
    return in_range(reset_addr, slot_base, (uint32_t)M65_BOOTLOADER_SLOT_SIZE);
}

__attribute__((noreturn))
static void jump_to_slot(uint32_t slot_base)
{
    const uint32_t *vectors = (const uint32_t *)(slot_base + PICO_BOOT2_LEN);
    uint32_t initial_sp = vectors[0];
    uint32_t reset = vectors[1];

    __asm volatile("cpsid i");
    ppb_hw->syst_csr = 0;
    ppb_hw->nvic_icer = 0xffffffffu;
    ppb_hw->nvic_icpr = 0xffffffffu;
    scb_hw->vtor = (uint32_t)vectors;
    __asm volatile(
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r"(initial_sp), "r"(reset)
        : "memory"
    );
    __builtin_unreachable();
}

int main(void)
{
    uint32_t slot = slot0_base();
    if (slot_vectors_look_valid(slot)) {
        jump_to_slot(slot);
    }

    reset_usb_boot(0, 0);
    while (true) {
        tight_loop_contents();
    }
}
