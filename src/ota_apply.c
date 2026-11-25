// Stages a firmware update from the SD card into flash slot 1 and asks the
// resident bootloader to install it (see include/ota_record.h).
//
// Nothing is written to flash until the whole UF2 file has been checked, and
// the bootloader is only asked to install once slot 1 has been read back and
// matches. A bad, truncated or wrong-chip file therefore fails here with a
// message, and the running firmware is never touched.

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "storage.h"
#include "remote_http.h"
#include "config.h"
#include "ota_record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u

#define UF2_FLAG_NOT_MAIN_FLASH  0x00000001u
#define UF2_FLAG_FAMILY_PRESENT  0x00002000u

#define UF2_FAMILY_RP2040        0xE48BFF56u
#define UF2_FAMILY_RP2350_ARM_S  0xE48BFF59u

#if PICO_RP2040
#define UF2_FAMILY_THIS_CHIP UF2_FAMILY_RP2040
#define THIS_CHIP_NAME       "RP2040 (Pico W)"
#define IMAGE_VECTOR_OFFSET  0x100u   // RP2040 images start with 256 bytes of boot2
#else
#define UF2_FAMILY_THIS_CHIP UF2_FAMILY_RP2350_ARM_S
#define THIS_CHIP_NAME       "RP2350 (Pico 2 W)"
#define IMAGE_VECTOR_OFFSET  0x0u
#endif

#define UF2_PAYLOAD 256u
#define UF2_BLOCK   512u
#define SIG_TRAILER 256u   // M65_SIG_BLOCK_SIZE: signed trailer kept on mirror packages

// Bytes of UF2 data left in the package; the signed trailer after it is not
// part of the image
static uint32_t pkg_remaining;

_Static_assert(M65_OTA_SECTOR_SIZE == FLASH_SECTOR_SIZE, "sector size mismatch");
_Static_assert(M65_OTA_SLOT1_OFFSET + M65_OTA_SLOT_SIZE <= PICO_FLASH_SIZE_BYTES,
               "slot 1 does not fit in this board's flash");

typedef struct {
    uint32_t magicStart0;
    uint32_t magicStart1;
    uint32_t flags;
    uint32_t targetAddr;
    uint32_t payloadSize;
    uint32_t blockNo;
    uint32_t numBlocks;
    uint32_t fileSize;   // family ID when UF2_FLAG_FAMILY_PRESENT
    uint8_t data[476];
    uint32_t magicEnd;
} uf2_block_t;

_Static_assert(sizeof(uf2_block_t) == 512, "UF2 blocks are 512 bytes");

#define SLOT0_ADDR (XIP_BASE + M65_OTA_SLOT0_OFFSET)

typedef enum {
    BLOCK_OTHER,   // separate block stream: another chip's image in a universal
                   // UF2, the RP2350 errata block, non-flash data
    BLOCK_SKIP,    // this chip's stream, but not for the slot: the bootloader
                   // part of a factory UF2
    BLOCK_IMAGE,   // this chip's stream: firmware image for slot 0
} block_kind_t;

static bool fail(char *err, size_t err_len, const char *fmt, unsigned long a, unsigned long b)
{
    if (err && err_len) snprintf(err, err_len, fmt, a, b);
    return false;
}

// Classifies one block, or fails with a reason. Shared by the check pass and
// the write pass so both agree exactly on what goes into the slot.
static bool classify_block(const uf2_block_t *b, uint32_t index, block_kind_t *kind,
                           char *err, size_t err_len)
{
    if (b->magicStart0 != UF2_MAGIC_START0 || b->magicStart1 != UF2_MAGIC_START1 ||
        b->magicEnd != UF2_MAGIC_END) {
        return fail(err, err_len, "not a valid UF2 file (bad block %lu)", index, 0);
    }
    if (b->flags & UF2_FLAG_NOT_MAIN_FLASH) {
        *kind = BLOCK_OTHER;
        return true;
    }
    if (!(b->flags & UF2_FLAG_FAMILY_PRESENT)) {
        return fail(err, err_len, "UF2 file does not say which chip it is for", 0, 0);
    }
    if (b->fileSize != UF2_FAMILY_THIS_CHIP) {
        // Another chip's image (universal UF2) or the errata block: not ours.
        // check_package() still requires a complete image for this chip.
        *kind = BLOCK_OTHER;
        return true;
    }
    if (b->payloadSize != UF2_PAYLOAD || (b->targetAddr % UF2_PAYLOAD) != 0) {
        return fail(err, err_len, "unsupported UF2 block layout (block %lu)", index, 0);
    }

    uint32_t a = b->targetAddr;
    if (a >= XIP_BASE && a + UF2_PAYLOAD <= SLOT0_ADDR) {
        // Factory UF2s also carry the resident bootloader; OTA never rewrites it
        *kind = BLOCK_SKIP;
        return true;
    }
    if (a >= SLOT0_ADDR && a + UF2_PAYLOAD <= SLOT0_ADDR + M65_OTA_SLOT_SIZE) {
        *kind = BLOCK_IMAGE;
        return true;
    }
    return fail(err, err_len,
                "firmware does not fit the update slot (address %08lx); is it a factory build?",
                (unsigned long)a, 0);
}

// Pico SDK images start with the stack at the very top of RAM (SRAM_END) and
// a Thumb reset handler inside the slot. Requiring both exactly means a plain
// (non-factory) build, linked at the start of flash, cannot pass by chance:
// at the slot offset it has ordinary code, not a vector table.
static bool vectors_plausible(const uint8_t *p)
{
    uint32_t sp, reset;
    memcpy(&sp, p, 4);
    memcpy(&reset, p + 4, 4);
    if (sp != SRAM_END) return false;
    if (!(reset & 1u)) return false;
    reset &= ~1u;
    return reset >= SLOT0_ADDR && reset < SLOT0_ADDR + M65_OTA_SLOT_SIZE;
}

static bool read_block(storage_file_t *f, uf2_block_t *b, bool *eof, char *err, size_t err_len)
{
    if (pkg_remaining == 0) {
        *eof = true;
        return true;
    }
    pkg_remaining -= UF2_BLOCK;
    size_t got = 0;
    if (!storage_read(f, b, sizeof *b, &got)) {
        return fail(err, err_len, "SD read failed", 0, 0);
    }
    *eof = (got == 0);
    if (got != 0 && got != sizeof *b) {
        return fail(err, err_len, "firmware file is truncated (partial UF2 block)", 0, 0);
    }
    return true;
}

// Pass 1: read the whole file and check it, without touching flash.
// On success *image_len is the image size from the start of the slot,
// rounded up to whole sectors.
static bool check_package(storage_file_t *f, uf2_block_t *b, uint32_t *image_len,
                          char *err, size_t err_len)
{
    // UF2 numbers blocks separately for each chip family, so only this
    // chip's blocks form the sequence that must be complete and in order
    uint32_t index = 0, seq = 0, num_blocks = 0, image_blocks = 0;
    uint32_t next_image_addr = SLOT0_ADDR;
    bool vectors_seen = false;

    for (;;) {
        bool eof;
        if (!read_block(f, b, &eof, err, err_len)) return false;
        if (eof) break;

        block_kind_t kind;
        if (!classify_block(b, index, &kind, err, err_len)) return false;

        if (kind == BLOCK_OTHER) {
            index++;
            continue;
        }
        if (seq == 0) num_blocks = b->numBlocks;
        if (b->blockNo != seq || b->numBlocks != num_blocks) {
            return fail(err, err_len, "UF2 blocks out of order or missing (block %lu)", index, 0);
        }
        seq++;

        if (kind == BLOCK_IMAGE) {
            // In address order and starting at the slot, so the write pass can
            // stream whole sectors; gaps are allowed and become 0xFF
            if (image_blocks == 0 && b->targetAddr != SLOT0_ADDR) {
                return fail(err, err_len, "firmware image does not start at the slot (%08lx)",
                            (unsigned long)b->targetAddr, 0);
            }
            if (b->targetAddr < next_image_addr) {
                return fail(err, err_len, "UF2 blocks out of address order (block %lu)", index, 0);
            }
            next_image_addr = b->targetAddr + UF2_PAYLOAD;
            if (b->targetAddr == SLOT0_ADDR + (IMAGE_VECTOR_OFFSET & ~(UF2_PAYLOAD - 1u))) {
                if (!vectors_plausible(b->data + (IMAGE_VECTOR_OFFSET % UF2_PAYLOAD))) {
                    return fail(err, err_len, "firmware image has invalid start-up vectors", 0, 0);
                }
                vectors_seen = true;
            }
            image_blocks++;
        }
        index++;
    }

    if (seq == 0) {
        return fail(err, err_len, "firmware is for a different chip (no " THIS_CHIP_NAME " image in the file)", 0, 0);
    }
    if (seq != num_blocks) {
        return fail(err, err_len, "firmware file is truncated (%lu of %lu blocks)", seq, num_blocks);
    }
    if (!image_blocks) return fail(err, err_len, "no firmware image in the file", 0, 0);
    if (!vectors_seen) return fail(err, err_len, "firmware image has no start-up vectors", 0, 0);

    uint32_t len = next_image_addr - SLOT0_ADDR;
    *image_len = (len + M65_OTA_SECTOR_SIZE - 1u) & ~(M65_OTA_SECTOR_SIZE - 1u);
    return true;
}

static void flash_write_sector(uint32_t offset, const uint8_t *data)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

// Pass 2: write the image into slot 1 sector by sector. Every sector up to
// image_len is written (gaps as 0xFF), so slot 1 holds exactly the image.
// *crc_out is the CRC of what was written.
static bool write_slot1(storage_file_t *f, uint32_t uf2_len, uf2_block_t *b, uint8_t *sector,
                        uint32_t image_len, uint32_t *crc_out, char *err, size_t err_len)
{
    if (!storage_seek(f, 0)) return fail(err, err_len, "SD seek failed", 0, 0);
    pkg_remaining = uf2_len;

    uint32_t crc = 0, cur = 0, index = 0;
    memset(sector, 0xFF, FLASH_SECTOR_SIZE);

    for (;;) {
        bool eof;
        if (!read_block(f, b, &eof, err, err_len)) return false;
        if (eof) break;
        block_kind_t kind;
        if (!classify_block(b, index++, &kind, err, err_len)) return false;
        if (kind != BLOCK_IMAGE) continue;

        uint32_t off = b->targetAddr - SLOT0_ADDR;
        while (off >= cur + FLASH_SECTOR_SIZE) {
            flash_write_sector(M65_OTA_SLOT1_OFFSET + cur, sector);
            crc = m65_crc32_update(crc, sector, FLASH_SECTOR_SIZE);
            cur += FLASH_SECTOR_SIZE;
            memset(sector, 0xFF, FLASH_SECTOR_SIZE);
        }
        memcpy(sector + (off - cur), b->data, UF2_PAYLOAD);
    }
    while (cur < image_len) {
        flash_write_sector(M65_OTA_SLOT1_OFFSET + cur, sector);
        crc = m65_crc32_update(crc, sector, FLASH_SECTOR_SIZE);
        cur += FLASH_SECTOR_SIZE;
        memset(sector, 0xFF, FLASH_SECTOR_SIZE);
    }
    *crc_out = crc;
    return true;
}

static bool write_record(uint8_t *sector, uint32_t image_len, uint32_t image_crc)
{
    m65_ota_record_t rec = {
        .magic = M65_OTA_RECORD_MAGIC,
        .version = M65_OTA_RECORD_VERSION,
        .image_len = image_len,
        .image_crc = image_crc,
    };
    rec.record_crc = m65_ota_record_crc(&rec);
    memset(sector, 0xFF, FLASH_SECTOR_SIZE);
    memcpy(sector, &rec, sizeof rec);
    flash_write_sector(M65_OTA_RECORD_OFFSET, sector);
    return memcmp((const void *)(XIP_BASE + M65_OTA_RECORD_OFFSET), &rec, sizeof rec) == 0;
}

bool remote_http_firmware_update(char *err, size_t err_len)
{
    if (err && err_len) err[0] = 0;
    if (!firmware_update_available()) {
        return fail(err, err_len, "no pending firmware package", 0, 0);
    }
    if (!remote_http_verify_firmware_package(M65_FIRMWARE_PACKAGE_PATH, err, err_len)) return false;

    // Heap, not stack: the main stack is only 2 KB
    uf2_block_t *block = (uf2_block_t *)malloc(sizeof *block);
    uint8_t *sector = (uint8_t *)malloc(FLASH_SECTOR_SIZE);
    if (!block || !sector) {
        free(block);
        free(sector);
        return fail(err, err_len, "not enough memory to stage the update", 0, 0);
    }

    bool ok = false;
    storage_file_t f;
    if (!storage_open(&f, M65_FIRMWARE_PACKAGE_PATH)) {
        fail(err, err_len, "cannot open the firmware package", 0, 0);
    } else {
        // A UF2 file is whole 512-byte blocks; a mirror package has the
        // 256-byte signed trailer after them
        uint32_t size = storage_size(&f), uf2_len = 0;
        uint32_t image_len = 0, written_crc = 0;
        if (size % UF2_BLOCK == 0) uf2_len = size;
        else if (size % UF2_BLOCK == SIG_TRAILER) uf2_len = size - SIG_TRAILER;
        pkg_remaining = uf2_len;
        if (size % UF2_BLOCK != 0 && size % UF2_BLOCK != SIG_TRAILER) {
            fail(err, err_len, "firmware file is truncated or corrupt (%lu bytes)", size, 0);
        } else if (check_package(&f, block, &image_len, err, err_len) &&
            write_slot1(&f, uf2_len, block, sector, image_len, &written_crc, err, err_len)) {
            uint32_t readback = m65_crc32_update(0, (const uint8_t *)(XIP_BASE + M65_OTA_SLOT1_OFFSET),
                                                 image_len);
            if (readback != written_crc) {
                fail(err, err_len, "flash verify failed (slot 1 read back wrong)", 0, 0);
            } else if (!write_record(sector, image_len, written_crc)) {
                fail(err, err_len, "flash verify failed (install record)", 0, 0);
            } else {
                ok = true;
            }
        }
        storage_close(&f);
    }

    free(block);
    free(sector);
    return ok;
}

static int64_t reboot_alarm(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;
    // Older bootloaders only understand the scratch flag; newer ones install
    // from the record and ignore it
    watchdog_hw->scratch[2] = M65_OTA_LEGACY_SCRATCH_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
    return 0;
}

void remote_http_firmware_reboot(uint32_t delay_ms)
{
    // Gives the caller's reply (HTTP response or AT OK) time to get out
    if (add_alarm_in_ms(delay_ms, reboot_alarm, NULL, true) < 0) reboot_alarm(0, NULL);
}
