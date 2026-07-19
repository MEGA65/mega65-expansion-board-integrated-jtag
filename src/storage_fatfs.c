#include "storage.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

// This file expects a FatFs diskio layer to be provided by your chosen Pico SD
// driver. The common carlk3/no-OS-FatFS-SD-SPI-RPi-Pico project provides ff.h,
// diskio.h, and optionally sd_init_driver().
#include "ff.h"
#if __has_include("diskio.h")
#include "diskio.h"
#endif
#if __has_include("sd_driver/sd_card.h")
#include "sd_driver/sd_card.h"
#endif

#if __has_include("hw_config.h")
#include "hw_config.h"
#endif

static FATFS fs;
#define STORAGE_FILE_SLOTS 4u
static FIL file_pool[STORAGE_FILE_SLOTS];
static bool file_pool_used[STORAGE_FILE_SLOTS];
static char last_err[128];
static bool mounted;

static void set_err(const char *where, FRESULT fr)
{
    snprintf(last_err, sizeof(last_err), "%s: FatFs error %d transport=%s",
             where, (int)fr, storage_sd_transport_name());
}

static void reset_sd_driver_state(void)
{
#if __has_include("hw_config.h") && defined(STA_NOINIT)
    for (size_t i = 0; i < sd_get_num(); ++i) {
        sd_card_t *sd = sd_get_by_num(i);
        if (sd) {
            sd->m_Status |= STA_NOINIT;
            sd->mounted = false;
        }
    }
#endif
}

const char *storage_last_error(void)
{
    return last_err[0] ? last_err : "no storage error";
}

static FIL *alloc_file(void)
{
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (!file_pool_used[i]) {
            file_pool_used[i] = true;
            memset(&file_pool[i], 0, sizeof file_pool[i]);
            return &file_pool[i];
        }
    }
    snprintf(last_err, sizeof(last_err), "no FatFs file slots");
    return NULL;
}

static void free_file(FIL *f)
{
    if (!f) return;
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (f == &file_pool[i]) {
            file_pool_used[i] = false;
            memset(&file_pool[i], 0, sizeof file_pool[i]);
            return;
        }
    }
}

bool storage_mount(void)
{
    if (mounted) return true;
    if (!storage_sd_may_mount()) {
        snprintf(last_err, sizeof(last_err), "not-present");
        return false;
    }
#if defined(HAVE_SD_INIT_DRIVER)
    if (!sd_init_driver()) {
        snprintf(last_err, sizeof(last_err), "sd_init_driver failed");
        return false;
    }
#endif
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        f_unmount("");
        reset_sd_driver_state();
        storage_sd_probe();
        sleep_ms(20);
        fr = f_mount(&fs, "", 1);
    }
    if (fr != FR_OK) { set_err("f_mount", fr); return false; }
    last_err[0] = 0;
    mounted = true;
    return true;
}

void storage_unmount(void)
{
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (file_pool_used[i]) {
            f_close(&file_pool[i]);
            file_pool_used[i] = false;
            memset(&file_pool[i], 0, sizeof file_pool[i]);
        }
    }
    f_unmount("");
    mounted = false;
}

bool storage_is_mounted(void)
{
    return mounted;
}

bool storage_open(storage_file_t *f, const char *path)
{
    if (!f || !path) return false;
    FIL *slot = alloc_file();
    if (!slot) return false;
    FRESULT fr = f_open(slot, path, FA_READ);
    if (fr != FR_OK) {
        free_file(slot);
        set_err("f_open", fr);
        return false;
    }
    f->impl = slot;
    return true;
}

bool storage_open_write(storage_file_t *f, const char *path, bool truncate)
{
    if (!f || !path) return false;
    BYTE mode = FA_WRITE | FA_CREATE_ALWAYS;
    if (!truncate) mode = FA_WRITE | FA_OPEN_ALWAYS;
    FIL *slot = alloc_file();
    if (!slot) return false;
    FRESULT fr = f_open(slot, path, mode);
    if (fr != FR_OK) {
        free_file(slot);
        set_err("f_open_write", fr);
        return false;
    }
    f->impl = slot;
    return true;
}

bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got)
{
    if (!f || !f->impl || !buf) return false;
    UINT br = 0;
    FRESULT fr = f_read((FIL *)f->impl, buf, (UINT)len, &br);
    if (got) *got = (size_t)br;
    if (fr != FR_OK) { set_err("f_read", fr); return false; }
    return true;
}

bool storage_write(storage_file_t *f, const void *buf, size_t len, size_t *put)
{
    if (!f || !f->impl || !buf) return false;
    UINT bw = 0;
    FRESULT fr = f_write((FIL *)f->impl, buf, (UINT)len, &bw);
    if (put) *put = (size_t)bw;
    if (fr != FR_OK) { set_err("f_write", fr); return false; }
    return true;
}

bool storage_sync(storage_file_t *f)
{
    if (!f || !f->impl) return false;
    FRESULT fr = f_sync((FIL *)f->impl);
    if (fr != FR_OK) { set_err("f_sync", fr); return false; }
    return true;
}

bool storage_seek(storage_file_t *f, uint32_t offset)
{
    if (!f || !f->impl) return false;
    FRESULT fr = f_lseek((FIL *)f->impl, (FSIZE_t)offset);
    if (fr != FR_OK) { set_err("f_lseek", fr); return false; }
    return true;
}

uint32_t storage_tell(storage_file_t *f)
{
    if (!f || !f->impl) return 0;
    return (uint32_t)f_tell((FIL *)f->impl);
}

uint32_t storage_size(storage_file_t *f)
{
    if (!f || !f->impl) return 0;
    return (uint32_t)f_size((FIL *)f->impl);
}

void storage_close(storage_file_t *f)
{
    if (f && f->impl) {
        FIL *slot = (FIL *)f->impl;
        f_close(slot);
        free_file(slot);
        f->impl = NULL;
    }
}

static bool has_core_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char e1 = (char)tolower((unsigned char)dot[1]);
    char e2 = (char)tolower((unsigned char)dot[2]);
    char e3 = (char)tolower((unsigned char)dot[3]);
    char e4 = (char)tolower((unsigned char)dot[4]);
    char e5 = (char)tolower((unsigned char)dot[5]);
    return (e1 == 'b' && e2 == 'i' && e3 == 't' && e4 == 0) ||
           (e1 == 'c' && e2 == 'o' && e3 == 'r' && e4 == 0) ||
           (e1 == 'm' && e2 == '6' && e3 == '5' && e4 == 'j' && e5 == 0);
}

static bool ext_equal_ci_local(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool has_theme_ext(const char *name)
{
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot) return false;
    return ext_equal_ci_local(dot, ".m65jtheme") || ext_equal_ci_local(dot, ".tar");
}

static bool has_partial_core_ext(const char *name)
{
    size_t n = strlen(name ? name : "");
    static const char suffix[] = ".partial";
    size_t sn = sizeof suffix - 1u;
    if (n <= sn) return false;
    const char *tail = name + n - sn;
    for (size_t i = 0; i < sn; i++) {
        if (tolower((unsigned char)tail[i]) != suffix[i]) return false;
    }
    char base[256];
    if (n - sn >= sizeof base) return false;
    memcpy(base, name, n - sn);
    base[n - sn] = 0;
    return has_core_ext(base);
}

bool storage_delete(const char *path)
{
    if (!path) return false;
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK && fr != FR_NO_FILE) { set_err("f_unlink", fr); return false; }
    return true;
}

bool storage_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return false;
    FRESULT fr = f_rename(old_path, new_path);
    if (fr != FR_OK) { set_err("f_rename", fr); return false; }
    return true;
}

bool storage_mkdir(const char *path)
{
    if (!path || !path[0]) return false;
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK && fr != FR_EXIST) { set_err("f_mkdir", fr); return false; }
    return true;
}

static bool storage_list_filtered(const char *path, storage_list_cb_t cb, void *ctx, bool cores_only)
{
    DIR dir;
    FILINFO fi;
    const char *p = (path && path[0]) ? path : "/";
    FRESULT fr = f_opendir(&dir, p);
    if (fr != FR_OK) { set_err("f_opendir", fr); return false; }
    bool in_theme_dir = ext_equal_ci_local(p, M65_THEME_DIR_PATH) ||
                        (p[0] == '/' && ext_equal_ci_local(p + 1, M65_THEME_DIR_PATH));

    for (;;) {
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK) { set_err("f_readdir", fr); f_closedir(&dir); return false; }
        if (fi.fname[0] == 0) break;
        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (!cores_only || is_dir || has_core_ext(fi.fname) ||
            (in_theme_dir && has_theme_ext(fi.fname)) ||
            has_partial_core_ext(fi.fname)) {
            cb(fi.fname, (uint32_t)fi.fsize, is_dir, ctx);
        }
    }
    f_closedir(&dir);
    return true;
}

bool storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx)
{
    return storage_list_filtered(path, cb, ctx, true);
}

bool storage_list_dir(const char *path, storage_list_cb_t cb, void *ctx)
{
    return storage_list_filtered(path, cb, ctx, false);
}
