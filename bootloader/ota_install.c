// Power-safe install of a staged update (see include/ota_record.h).

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"

#include "boot.h"

_Static_assert(M65_OTA_SECTOR_SIZE == FLASH_SECTOR_SIZE, "sector size mismatch");

#define OTA_COPY_ATTEMPTS 3

// Sector staging buffer: flash cannot be read through XIP while it is being
// programmed, so each sector is copied to RAM first
static uint8_t sector_buf[FLASH_SECTOR_SIZE];

static uint32_t flash_crc(uint32_t offset, uint32_t len)
{
    return m65_crc32_update(0, (const uint8_t *)(XIP_BASE_ADDR + offset), len);
}

static void flash_write_sector(uint32_t offset, const uint8_t *data)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

static void erase_record(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(M65_OTA_RECORD_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

static void write_record(uint32_t image_len, uint32_t image_crc)
{
    m65_ota_record_t rec = {
        .magic = M65_OTA_RECORD_MAGIC,
        .version = M65_OTA_RECORD_VERSION,
        .image_len = image_len,
        .image_crc = image_crc,
    };
    rec.record_crc = m65_ota_record_crc(&rec);
    memset(sector_buf, 0xFF, sizeof sector_buf);
    memcpy(sector_buf, &rec, sizeof rec);
    flash_write_sector(M65_OTA_RECORD_OFFSET, sector_buf);
}

// Called only with a valid record in flash. Slot 0 is not touched until slot
// 1 has been verified, and the record is erased only once slot 0 verifies.
// If power fails part way through, the record survives and the copy is
// simply repeated on the next boot.
static void install_from_record(void)
{
    const m65_ota_record_t *flash_rec =
        (const m65_ota_record_t *)(XIP_BASE_ADDR + M65_OTA_RECORD_OFFSET);
    const uint32_t len = flash_rec->image_len;
    const uint32_t crc = flash_rec->image_crc;

    diag_stage(4, "ota-start");

    if (flash_crc(M65_OTA_SLOT1_OFFSET, len) != crc) {
        // Staged image is damaged. Slot 0 has not been touched by this
        // record, so drop the request and boot what is there.
        diag_puts("M65BOOT staged image fails CRC, update abandoned\r\n");
        erase_record();
        return;
    }

    for (int attempt = 0;; attempt++) {
        // Checked before the first copy too: if power failed after the copy
        // but before the record was erased, there is nothing left to do
        if (flash_crc(M65_OTA_SLOT0_OFFSET, len) == crc) {
            erase_record();
            diag_stage(5, "ota-done");
            return;
        }
        if (attempt == OTA_COPY_ATTEMPTS) break;
        for (uint32_t off = 0; off < len; off += FLASH_SECTOR_SIZE) {
            memcpy(sector_buf, (const void *)(XIP_BASE_ADDR + M65_OTA_SLOT1_OFFSET + off), FLASH_SECTOR_SIZE);
            flash_write_sector(M65_OTA_SLOT0_OFFSET + off, sector_buf);
        }
    }

    // Still not right after several attempts. Keep the record so the next
    // boot tries again; the slot 0 check below decides whether to run it.
    diag_puts("M65BOOT copy did not verify; will retry next boot\r\n");
}

// Older firmware requested an install by watchdog scratch only, with no CRC.
// Accept it if the staged image looks like firmware linked for slot 0, and
// turn it into a record so the copy gets the same power-loss protection.
static void accept_legacy_request(void)
{
    uint32_t slot1 = XIP_BASE_ADDR + M65_OTA_SLOT1_OFFSET;
    if (!image_vectors_look_valid(slot1, slot0_base())) {
        diag_puts("M65BOOT legacy request ignored: slot 1 does not look like firmware\r\n");
        return;
    }
    write_record(M65_OTA_SLOT_SIZE, flash_crc(M65_OTA_SLOT1_OFFSET, M65_OTA_SLOT_SIZE));
}

void ota_boot_check(bool legacy_request)
{
    const m65_ota_record_t *rec = (const m65_ota_record_t *)(XIP_BASE_ADDR + M65_OTA_RECORD_OFFSET);
    if (!m65_ota_record_valid(rec) && legacy_request) accept_legacy_request();
    if (m65_ota_record_valid(rec)) install_from_record();
}
