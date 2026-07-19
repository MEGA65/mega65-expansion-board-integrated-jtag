#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "storage.h"
#include "remote_http.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

#define UF2_MAGIC_START0 0x0A324655
#define UF2_MAGIC_START1 0x9E5D5157
#define UF2_MAGIC_END    0x0AB16F30

struct uf2_block {
    uint32_t magicStart0;
    uint32_t magicStart1;
    uint32_t flags;
    uint32_t targetAddr;
    uint32_t payloadSize;
    uint32_t blockNo;
    uint32_t numBlocks;
    uint32_t fileSize;
    uint8_t data[476];
    uint32_t magicEnd;
};

#define SLOT0_ADDR (XIP_BASE + M65_BOOTLOADER_SLOT0_OFFSET)
#define SLOT1_ADDR (XIP_BASE + M65_BOOTLOADER_SLOT0_OFFSET + M65_BOOTLOADER_SLOT_SIZE)

bool remote_http_firmware_update(char *err, size_t err_len)
{
    if (!firmware_update_available()) {
        if (err && err_len) snprintf(err, err_len, "no pending firmware package");
        return false;
    }

    storage_file_t f;
    if (!storage_open(&f, M65_FIRMWARE_PACKAGE_PATH)) {
        if (err && err_len) snprintf(err, err_len, "failed to open %s", M65_FIRMWARE_PACKAGE_PATH);
        return false;
    }

    uint8_t sector_buf[FLASH_SECTOR_SIZE];
    memset(sector_buf, 0xFF, sizeof(sector_buf));
    uint32_t current_sector = 0xFFFFFFFF;
    bool sector_dirty = false;

    struct uf2_block block;
    size_t bytes_read;
    bool success = true;

    while (storage_read(&f, &block, sizeof(block), &bytes_read) && bytes_read == sizeof(block)) {
        if (block.magicStart0 != UF2_MAGIC_START0 ||
            block.magicStart1 != UF2_MAGIC_START1 ||
            block.magicEnd != UF2_MAGIC_END) {
            continue; // Skip invalid blocks
        }

        if (block.targetAddr >= SLOT0_ADDR && block.targetAddr < SLOT0_ADDR + M65_BOOTLOADER_SLOT_SIZE) {
            uint32_t offset_in_slot = block.targetAddr - SLOT0_ADDR;
            uint32_t slot1_target = (SLOT1_ADDR - XIP_BASE) + offset_in_slot;

            uint32_t sector_start = slot1_target & ~(FLASH_SECTOR_SIZE - 1);
            uint32_t offset_in_sector = slot1_target & (FLASH_SECTOR_SIZE - 1);

            if (current_sector != sector_start) {
                if (sector_dirty) {
                    uint32_t ints = save_and_disable_interrupts();
                    flash_range_erase(current_sector, FLASH_SECTOR_SIZE);
                    flash_range_program(current_sector, sector_buf, FLASH_SECTOR_SIZE);
                    restore_interrupts(ints);
                }
                current_sector = sector_start;
                memset(sector_buf, 0xFF, sizeof(sector_buf));
                sector_dirty = false;
            }

            uint32_t copy_size = block.payloadSize;
            if (copy_size > 256) copy_size = 256;
            if (offset_in_sector + copy_size <= FLASH_SECTOR_SIZE) {
                memcpy(&sector_buf[offset_in_sector], block.data, copy_size);
                sector_dirty = true;
            }
        }
    }

    if (sector_dirty) {
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(current_sector, FLASH_SECTOR_SIZE);
        flash_range_program(current_sector, sector_buf, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
    }

    storage_close(&f);

    if (success) {
        // Set watchdog scratch to tell bootloader to copy slot 1 to slot 0
        watchdog_hw->scratch[2] = 0x07A07A07;
        watchdog_reboot(0, 0, 100);
        for (;;) tight_loop_contents();
    }

    return false;
}
