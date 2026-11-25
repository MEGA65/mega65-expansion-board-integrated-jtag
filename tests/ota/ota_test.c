// Host test for the OTA path: src/ota_apply.c (firmware side: check a UF2
// package and stage it into slot 1) and bootloader/ota_install.c (install it
// into slot 0). Both are compiled unchanged against tiny SDK stand-ins.
//
// Flash is an anonymous mapping at the real XIP address, so the code's
// pointer reads of flash work as on the Pico. The flash driver stub can cut
// the power at any chosen erase/program operation, leaving that operation
// half done, which is how the power-loss sweeps below work.
//
// Usage: ota_test <factory.uf2> <app.bin> <other-chip-factory.uf2> <plain.uf2>
//   factory.uf2  factory image for the chip this test was built for
//   app.bin      the app inside it (pico_m65jtag.bin from the factory build)
//   other-chip   a factory image for the other chip (must be refused)
//   plain.uf2    a plain, non-factory image for this chip (must be refused)

#define _GNU_SOURCE
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "storage.h"
#include "remote_http.h"
#include "ota_record.h"

// bootloader/boot.h
void ota_boot_check(bool legacy_request);

#define FLASH ((uint8_t *)(uintptr_t)XIP_BASE)
#define SLOT0 (FLASH + M65_OTA_SLOT0_OFFSET)
#define SLOT1 (FLASH + M65_OTA_SLOT1_OFFSET)
#define RECORD ((const m65_ota_record_t *)(FLASH + M65_OTA_RECORD_OFFSET))

#if PICO_RP2040
#define BOOT2_LEN 0x100u
#define CHIP "RP2040"
#else
#define BOOT2_LEN 0x0u
#define CHIP "RP2350"
#endif

static int failures;
#define CHECK(cond, ...) do { if (cond) { printf("  ok    "); } else { printf("  FAIL  "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

// --- SDK stand-ins ---------------------------------------------------------

static watchdog_hw_t wd;
watchdog_hw_t *watchdog_hw = &wd;
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) { (void)pc; (void)sp; (void)delay_ms; }

static long op_count, cut_at;      // cut_at: power fails at this op (0 = never)
static jmp_buf power_cut;
static uint32_t rng = 12345;

static bool power_fails_now(void)
{
    return ++op_count == cut_at;
}

void flash_range_erase(uint32_t offset, size_t count)
{
    if (offset % FLASH_SECTOR_SIZE || count % FLASH_SECTOR_SIZE) { printf("  FAIL  unaligned erase\n"); failures++; }
    for (size_t s = 0; s < count; s += FLASH_SECTOR_SIZE) {
        uint8_t *p = FLASH + offset + s;
        if (power_fails_now()) {
            // A half-finished erase leaves the sector in no particular state
            for (size_t i = 0; i < FLASH_SECTOR_SIZE; i++) { rng = rng * 1103515245u + 12345u; p[i] = (uint8_t)(rng >> 16); }
            longjmp(power_cut, 1);
        }
        memset(p, 0xFF, FLASH_SECTOR_SIZE);
    }
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    if (offset % FLASH_PAGE_SIZE || count % FLASH_PAGE_SIZE) { printf("  FAIL  unaligned program\n"); failures++; }
    for (size_t pg = 0; pg < count; pg += FLASH_PAGE_SIZE) {
        uint8_t *p = FLASH + offset + pg;
        size_t n = FLASH_PAGE_SIZE;
        bool cut = power_fails_now();
        if (cut) n /= 2;
        for (size_t i = 0; i < n; i++) p[i] &= data[pg + i];   // flash can only clear bits
        if (cut) longjmp(power_cut, 1);
    }
}

const char *test_package_path;
bool firmware_update_available(void) { return true; }

bool storage_open(storage_file_t *f, const char *path) { f->fp = fopen(path, "rb"); return f->fp != NULL; }
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got) { *got = fread(buf, 1, len, f->fp); return !ferror(f->fp); }
bool storage_seek(storage_file_t *f, uint32_t offset) { return fseek(f->fp, (long)offset, SEEK_SET) == 0; }
void storage_close(storage_file_t *f) { if (f->fp) fclose(f->fp); f->fp = NULL; }
uint32_t storage_size(storage_file_t *f)
{
    long pos = ftell(f->fp);
    fseek(f->fp, 0, SEEK_END);
    long size = ftell(f->fp);
    fseek(f->fp, pos, SEEK_SET);
    return (uint32_t)size;
}

// Signature checking itself is signed_file.c's job (mbedTLS); here we only
// need to know the update refuses to go on when it fails
static bool signature_ok = true;
bool remote_http_verify_firmware_package(const char *path, char *err, size_t err_len)
{
    (void)path;
    if (!signature_ok) snprintf(err, err_len, "firmware signature check failed: test");
    return signature_ok;
}

// bootloader/main.c pieces used by ota_install.c
void diag_puts(const char *s) { if (getenv("OTA_TEST_VERBOSE")) printf("        boot: %s", s); }
void diag_stage(unsigned stage, const char *label) { (void)stage; (void)label; }
bool image_vectors_look_valid(uint32_t image_base, uint32_t link_base)
{
    const uint32_t *v = (const uint32_t *)(uintptr_t)(image_base + BOOT2_LEN);
    uint32_t reset = v[1] & ~1u;
    return !(v[0] & 3u) && v[0] >= SRAM_BASE && v[0] <= SRAM_END && (v[1] & 1u) &&
           reset >= link_base && reset < link_base + M65_OTA_SLOT_SIZE;
}

// --- helpers ---------------------------------------------------------------

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(2); }
    fseek(fp, 0, SEEK_END);
    *len = (size_t)ftell(fp);
    rewind(fp);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, fp) != *len) { perror(path); exit(2); }
    fclose(fp);
    return b;
}

static void write_file(const char *path, const uint8_t *b, size_t len)
{
    FILE *fp = fopen(path, "wb");
    fwrite(b, 1, len, fp);
    fclose(fp);
}

static uint8_t *old_fw;     // stands in for the firmware currently in slot 0
static uint8_t *app;        // the new firmware
static size_t app_len;

static void fresh_flash(void)
{
    memset(FLASH, 0xFF, PICO_FLASH_SIZE_BYTES);
    memcpy(SLOT0, old_fw, M65_OTA_SLOT_SIZE);
    memset(&wd, 0, sizeof wd);
}

static bool slot0_is_old(void) { return memcmp(SLOT0, old_fw, M65_OTA_SLOT_SIZE) == 0; }
static bool slot0_is_new(void) { return memcmp(SLOT0, app, app_len) == 0; }

static bool apply(const char *pkg, char *err)
{
    test_package_path = pkg;
    return remote_http_firmware_update(err, 160);
}

// Package must be refused with no flash writes at all
static void expect_refused(const char *what, const char *pkg, const char *want)
{
    char err[160];
    fresh_flash();
    op_count = 0;
    cut_at = 0;
    bool ok = apply(pkg, err);
    CHECK(!ok && strstr(err, want) && op_count == 0 && slot0_is_old(),
          "%s refused before any flash write: \"%s\"", what, err);
}

static void boot(bool legacy) { ota_boot_check(legacy); }

// --- tests -----------------------------------------------------------------

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s factory.uf2 app.bin other-chip-factory.uf2 plain.uf2\n", argv[0]);
        return 2;
    }
    if (mmap(FLASH, PICO_FLASH_SIZE_BYTES, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != FLASH) {
        perror("mmap at XIP_BASE");
        return 2;
    }
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char bad_path[512];
    snprintf(bad_path, sizeof bad_path, "%s/ota_test_bad_%d.uf2", tmp, (int)getpid());

    size_t pkg_len;
    uint8_t *pkg = read_file(argv[1], &pkg_len);
    app = read_file(argv[2], &app_len);
    old_fw = malloc(M65_OTA_SLOT_SIZE);
    for (uint32_t i = 0; i < M65_OTA_SLOT_SIZE; i++) old_fw[i] = (uint8_t)(i * 7u + 3u);
    char err[160];

    printf("%s: package check\n", CHIP);
    expect_refused("other chip's firmware", argv[3], "different chip");
    expect_refused("plain (non-factory) build", argv[4], "start-up vectors");
    write_file(bad_path, pkg, pkg_len - 512 * 100);
    expect_refused("truncated file", bad_path, "truncated");
    write_file(bad_path, pkg, pkg_len - 100);
    expect_refused("file cut mid-block", bad_path, "truncated");
    {
        uint8_t *b = malloc(pkg_len);
        memcpy(b, pkg, pkg_len);
        b[512 * 700] ^= 1;   // magic of block 700
        write_file(bad_path, b, pkg_len);
        expect_refused("corrupt block", bad_path, "not a valid UF2");
        memcpy(b, pkg, pkg_len);
        uint8_t t[512];
        memcpy(t, b + 512 * 600, 512);
        memcpy(b + 512 * 600, b + 512 * 601, 512);
        memcpy(b + 512 * 601, t, 512);
        write_file(bad_path, b, pkg_len);
        expect_refused("blocks out of order", bad_path, "out of order");
        free(b);
    }

    signature_ok = false;
    expect_refused("package failing its signature check", argv[1], "signature check failed");
    signature_ok = true;

    printf("%s: universal UF2 (both chips in one file)\n", CHIP);
    {
        size_t other_len;
        uint8_t *other = read_file(argv[3], &other_len);
        uint8_t *b = malloc(pkg_len + other_len + 256);
        for (int order = 0; order < 3; order++) {
            // 0: this chip first; 1: other chip first; 2: other first + signed trailer
            size_t len = pkg_len + other_len;
            if (order == 0) {
                memcpy(b, pkg, pkg_len);
                memcpy(b + pkg_len, other, other_len);
            } else {
                memcpy(b, other, other_len);
                memcpy(b + other_len, pkg, pkg_len);
            }
            if (order == 2) {
                memset(b + len, 0xA5, 256);
                len += 256;
            }
            write_file(bad_path, b, len);
            fresh_flash();
            op_count = 0;
            cut_at = 0;
            bool ok = apply(bad_path, err);
            CHECK(ok && memcmp(SLOT1, app, app_len) == 0 && m65_ota_record_valid(RECORD),
                  "%s staged this chip's image only (%s)",
                  order == 0 ? "this chip first" : order == 1 ? "other chip first" : "signed, other chip first",
                  ok ? "ok" : err);
        }
        // A universal file whose half for this chip is cut short
        memcpy(b, other, other_len);
        memcpy(b + other_len, pkg, pkg_len);
        write_file(bad_path, b, other_len + pkg_len - 512 * 50);
        expect_refused("universal file missing the end of this chip's image", bad_path, "truncated");
        free(b);
        free(other);
    }
    {
        // Mirror packages: the UF2 followed by a 256-byte signed trailer
        uint8_t *b = malloc(pkg_len + 256);
        memcpy(b, pkg, pkg_len);
        memset(b + pkg_len, 0xA5, 256);
        write_file(bad_path, b, pkg_len + 256);
        fresh_flash();
        op_count = 0;
        cut_at = 0;
        bool ok = apply(bad_path, err);
        CHECK(ok && memcmp(SLOT1, app, app_len) == 0 && m65_ota_record_valid(RECORD),
              "signed mirror package (UF2 + 256-byte trailer) staged (%s)", ok ? "ok" : err);
        free(b);
    }

    printf("%s: stage and install\n", CHIP);
    fresh_flash();
    op_count = 0;
    cut_at = 0;
    bool ok = apply(argv[1], err);
    CHECK(ok, "good package staged (%s)", ok ? "ok" : err);
    CHECK(slot0_is_old(), "staging leaves the running firmware alone");
    CHECK(memcmp(SLOT1, app, app_len) == 0, "slot 1 holds the app exactly");
    CHECK(m65_ota_record_valid(RECORD) && RECORD->image_len >= app_len &&
          RECORD->image_len < app_len + M65_OTA_SECTOR_SIZE, "install record written");
    long stage_ops = op_count;

    uint8_t *staged = malloc(PICO_FLASH_SIZE_BYTES);
    memcpy(staged, FLASH, PICO_FLASH_SIZE_BYTES);

    op_count = 0;
    boot(false);
    long install_ops = op_count;
    CHECK(slot0_is_new() && !m65_ota_record_valid(RECORD), "bootloader installs it and clears the record");
    op_count = 0;
    boot(false);
    CHECK(op_count == 0 && slot0_is_new(), "next boot does nothing");

    printf("%s: bootloader refuses a damaged staged image\n", CHIP);
    memcpy(FLASH, staged, PICO_FLASH_SIZE_BYTES);
    SLOT1[1000] ^= 0x40;
    boot(false);
    CHECK(slot0_is_old() && !m65_ota_record_valid(RECORD), "slot 0 untouched, request dropped");

    printf("%s: power cut at every bootloader flash operation (%ld ops)\n", CHIP, install_ops);
    {
        volatile int bad = 0, runs = 0;   // volatile: live across longjmp
        for (volatile long n = 1; n <= install_ops; n++) {
            memcpy(FLASH, staged, PICO_FLASH_SIZE_BYTES);
            op_count = 0;
            cut_at = n;
            if (!setjmp(power_cut)) boot(false);
            // After the cut: either the request survives or the install finished
            if (!m65_ota_record_valid(RECORD) && !slot0_is_new()) bad++;
            // Some cuts also hit the next boot, at a different point
            op_count = 0;
            cut_at = (n % 5 == 0) ? 1 + (n * 31) % install_ops : 0;
            if (cut_at && !setjmp(power_cut)) boot(false);
            op_count = 0;
            cut_at = 0;
            boot(false);
            if (!slot0_is_new() || m65_ota_record_valid(RECORD)) bad++;
            runs++;
        }
        CHECK(bad == 0, "%d power cuts, all recovered to the new firmware (%d bad)", runs, bad);
    }

    printf("%s: power cut while staging (%ld ops)\n", CHIP, stage_ops);
    {
        volatile int bad = 0, runs = 0;   // volatile: live across longjmp
        volatile long step = stage_ops > 400 ? stage_ops / 400 : 1;
        for (volatile long n = 1; n <= stage_ops; n += (n > stage_ops - 40 ? 1 : step)) {
            fresh_flash();
            op_count = 0;
            cut_at = n;
            if (!setjmp(power_cut)) apply(argv[1], err);
            op_count = 0;
            cut_at = 0;
            boot(false);
            // The update either did not happen, or happened completely
            if (!slot0_is_old() && !slot0_is_new()) bad++;
            if (m65_ota_record_valid(RECORD)) bad++;
            runs++;
        }
        CHECK(bad == 0, "%d power cuts, all left old or new firmware intact (%d bad)", runs, bad);
    }

    printf("%s: requests from older firmware (watchdog flag only)\n", CHIP);
    memcpy(FLASH, staged, PICO_FLASH_SIZE_BYTES);
    memset(FLASH + M65_OTA_RECORD_OFFSET, 0xFF, M65_OTA_SECTOR_SIZE);
    boot(true);
    CHECK(slot0_is_new() && !m65_ota_record_valid(RECORD), "valid staged image is installed");
    fresh_flash();
    memset(SLOT1, 0x5A, M65_OTA_SLOT_SIZE);
    boot(true);
    CHECK(slot0_is_old() && !m65_ota_record_valid(RECORD), "garbage in slot 1 is ignored");

    remove(bad_path);
    printf("%s: %s\n", CHIP, failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
