#pragma once

// Shared between the resident bootloader and the firmware: the "install the
// image in slot 1" request, and the CRC both sides use to check it.
//
// Flash layout (offsets from the start of flash):
//   0x00000  resident bootloader
//   RECORD   last sector before slot 0: the install record (this file)
//   SLOT0    running firmware
//   SLOT1    staged update, written by the firmware, copied by the bootloader
//
// The firmware writes slot 1, verifies it, then writes the record. The
// bootloader checks slot 1 against the record before erasing anything,
// copies it to slot 0 and verifies that, and only then erases the record.
// Power loss at any point leaves either the old firmware intact or the
// record in place, so the copy simply runs again on the next boot.
// A factory UF2 pads the bootloader region with 0xFF, so a fresh USB
// install always clears any stale record.

#include <stddef.h>
#include <stdint.h>

#ifndef M65_BOOTLOADER_SLOT0_OFFSET
#define M65_BOOTLOADER_SLOT0_OFFSET 0x10000u
#endif
#ifndef M65_BOOTLOADER_SLOT_SIZE
#define M65_BOOTLOADER_SLOT_SIZE 0xF0000u
#endif

#define M65_OTA_SECTOR_SIZE   4096u
#define M65_OTA_SLOT0_OFFSET  ((uint32_t)(M65_BOOTLOADER_SLOT0_OFFSET))
#define M65_OTA_SLOT_SIZE     ((uint32_t)(M65_BOOTLOADER_SLOT_SIZE))
#define M65_OTA_SLOT1_OFFSET  (M65_OTA_SLOT0_OFFSET + M65_OTA_SLOT_SIZE)
#define M65_OTA_RECORD_OFFSET (M65_OTA_SLOT0_OFFSET - M65_OTA_SECTOR_SIZE)

#define M65_OTA_RECORD_MAGIC   0x4F54414Du   /* "MATO" little-endian */
#define M65_OTA_RECORD_VERSION 1u

// Older firmware asked for an install by leaving this in watchdog scratch[2]
// and rebooting. New firmware sets it too, so an old bootloader still
// installs the update; a new bootloader prefers the record.
#define M65_OTA_LEGACY_SCRATCH_MAGIC 0x07A07A07u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t image_len;    // bytes from the start of the slot; whole sectors
    uint32_t image_crc;    // m65_crc32 of slot 1 over image_len
    uint32_t record_crc;   // m65_crc32 of the fields above
} m65_ota_record_t;

static inline uint32_t m65_crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline uint32_t m65_ota_record_crc(const m65_ota_record_t *r)
{
    return m65_crc32_update(0, (const uint8_t *)r, offsetof(m65_ota_record_t, record_crc));
}

static inline int m65_ota_record_valid(const m65_ota_record_t *r)
{
    return r->magic == M65_OTA_RECORD_MAGIC &&
           r->version == M65_OTA_RECORD_VERSION &&
           r->image_len != 0 &&
           r->image_len <= M65_OTA_SLOT_SIZE &&
           (r->image_len % M65_OTA_SECTOR_SIZE) == 0 &&
           r->record_crc == m65_ota_record_crc(r);
}
