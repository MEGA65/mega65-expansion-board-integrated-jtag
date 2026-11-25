#include "storage.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "uart_cmd.h"

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
// A slot whose volume went away (card removed or explicit unmount) while a
// caller still held it. It stays allocated, so the caller's handle can never
// alias a newer file, until the owner calls storage_close().
static bool file_pool_orphan[STORAGE_FILE_SLOTS];
static char last_err[128];
static bool mounted;
static uint32_t mount_gen;          // bumped whenever the volume comes or goes
static uint32_t change_gen;         // bumped whenever an entry is created, removed or renamed
static bool removal_event_pending;  // card lost; not yet reported by hotplug poll

static void card_lost(void);

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

/* ---- card detect ------------------------------------------------------ */

#if M65_SD_CARD_DETECT_ENABLED
#define CD_REMOVE_EDGE (M65_SD_CD_PRESENT_LEVEL ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE)

static bool cd_ready;
static bool cd_stable_present;
static bool cd_last_raw;
static absolute_time_t cd_changed_at;

static bool cd_raw_present(void)
{
    return gpio_get(M65_SD_CD_PIN) == (M65_SD_CD_PRESENT_LEVEL ? 1 : 0);
}

// io_bank0 INTR latches edge events even with the interrupt disabled, so a
// remove/reinsert that happens between polls (or during a long blocking
// operation) is still seen.
static bool cd_removal_edge_latched(void)
{
    uint32_t raw = io_bank0_hw->intr[M65_SD_CD_PIN / 8u] >> (4u * (M65_SD_CD_PIN % 8u));
    return (raw & CD_REMOVE_EDGE) != 0;
}

static void cd_clear_removal_edge(void)
{
    gpio_acknowledge_irq(M65_SD_CD_PIN, CD_REMOVE_EDGE);
}

static void cd_init(void)
{
    if (cd_ready) return;
    gpio_init(M65_SD_CD_PIN);
    gpio_set_dir(M65_SD_CD_PIN, GPIO_IN);
    if (M65_SD_CD_PRESENT_LEVEL) gpio_pull_down(M65_SD_CD_PIN);
    else gpio_pull_up(M65_SD_CD_PIN);
    busy_wait_us(100);  // let the pull settle before the first read
    cd_last_raw = cd_raw_present();
    cd_stable_present = cd_last_raw;
    cd_changed_at = get_absolute_time();
    cd_clear_removal_edge();
    cd_ready = true;
}
#endif

uint32_t storage_media_generation(void)
{
    // Changes on a card swap/remount *or* any change to the card's entries.
    // mount_gen alone still guards open directory walks (see list below),
    // which survive content changes but not a different volume.
    return mount_gen + change_gen;
}

bool storage_sd_card_present(void)
{
#if M65_SD_CARD_DETECT_ENABLED
    cd_init();
    return cd_raw_present();
#else
    return true;
#endif
}

// Guard for every FatFs entry point: fail fast (and drop the volume) if the
// card is gone, instead of waiting out SPI timeouts against an empty socket.
static bool card_ok(void)
{
#if M65_SD_CARD_DETECT_ENABLED
    cd_init();
    if (!cd_raw_present() || (mounted && cd_removal_edge_latched())) {
        card_lost();
        return false;
    }
#endif
    return true;
}

storage_sd_event_t storage_sd_hotplug_poll(void)
{
#if M65_SD_CARD_DETECT_ENABLED
    cd_init();
    if (mounted && cd_removal_edge_latched()) card_lost();

    bool raw = cd_raw_present();
    if (raw != cd_last_raw) {
        cd_last_raw = raw;
        cd_changed_at = get_absolute_time();
    }
    if (raw != cd_stable_present &&
        absolute_time_diff_us(cd_changed_at, get_absolute_time()) >= (int64_t)M65_SD_CD_DEBOUNCE_MS * 1000) {
        cd_stable_present = raw;
        if (!raw) {
            card_lost();
        } else if (!removal_event_pending) {
            return STORAGE_SD_EVENT_INSERTED;
        }
    }
#endif
    if (removal_event_pending) {
        removal_event_pending = false;
        return STORAGE_SD_EVENT_REMOVED;
    }
    return STORAGE_SD_EVENT_NONE;
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
            file_pool_orphan[i] = false;
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
            file_pool_orphan[i] = false;
            memset(&file_pool[i], 0, sizeof file_pool[i]);
            return;
        }
    }
}

static size_t slot_index(const void *impl)
{
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (impl == &file_pool[i]) return i;
    }
    return STORAGE_FILE_SLOTS;
}

// Resolve a caller's handle to a FIL that still belongs to the live volume.
static FIL *live_fil(storage_file_t *f)
{
    if (!f || !f->impl) return NULL;
    size_t i = slot_index(f->impl);
    if (i >= STORAGE_FILE_SLOTS || !file_pool_used[i]) return NULL;
    if (!card_ok() || file_pool_orphan[i]) {
        snprintf(last_err, sizeof(last_err), "sd card removed");
        return NULL;
    }
    return &file_pool[i];
}

static void orphan_open_files(bool close_first)
{
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (file_pool_used[i] && !file_pool_orphan[i]) {
            if (close_first) f_close(&file_pool[i]);
            file_pool_orphan[i] = true;
        }
    }
}

static void card_lost(void)
{
    bool had_files = false;
    for (size_t i = 0; i < STORAGE_FILE_SLOTS; ++i) {
        if (file_pool_used[i] && !file_pool_orphan[i]) had_files = true;
    }
    if (!mounted && !had_files) return;
    // Don't f_close(): there is no card to flush to, and the driver would
    // just time out. Handles are orphaned and freed by their owners.
    orphan_open_files(false);
    f_unmount("");
    reset_sd_driver_state();
    mounted = false;
    mount_gen++;
    removal_event_pending = true;
    snprintf(last_err, sizeof(last_err), "sd card removed");
}

bool storage_mount(void)
{
    if (mounted) {
        if (card_ok()) return true;
        return false;
    }
    if (removal_event_pending) {
        // Owners of the old card haven't been told yet. Their cleanup deletes
        // temp files by path, so it must not run against a newly mounted card.
        snprintf(last_err, sizeof(last_err), "sd card change pending");
        return false;
    }
    if (!storage_sd_card_present()) {
        snprintf(last_err, sizeof(last_err), "no card");
        return false;
    }
#if M65_SD_CARD_DETECT_ENABLED
    // Insertion bounce latches removal edges; anything before now is history.
    cd_clear_removal_edge();
#endif
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
    mount_gen++;
    return true;
}

void storage_unmount(void)
{
    // Close (flush) what is open while the card is still here, but keep the
    // slots reserved until their owners call storage_close().
    orphan_open_files(storage_sd_card_present());
    f_unmount("");
    if (mounted) mount_gen++;
    mounted = false;
}

bool storage_is_mounted(void)
{
    return mounted;
}

bool storage_open(storage_file_t *f, const char *path)
{
    if (!f || !path) return false;
    f->impl = NULL;
    if (!card_ok()) return false;
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
    f->impl = NULL;
    if (!card_ok()) return false;
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
    change_gen++;   // file created or truncated
    return true;
}

bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got)
{
    if (got) *got = 0;
    if (!buf) return false;
    FIL *fp = live_fil(f);
    if (!fp) return false;
    UINT br = 0;
    FRESULT fr = f_read(fp, buf, (UINT)len, &br);
    if (got) *got = (size_t)br;
    if (fr != FR_OK) { set_err("f_read", fr); return false; }
    return true;
}

bool storage_write(storage_file_t *f, const void *buf, size_t len, size_t *put)
{
    if (put) *put = 0;
    if (!buf) return false;
    FIL *fp = live_fil(f);
    if (!fp) return false;
    UINT bw = 0;
    FRESULT fr = f_write(fp, buf, (UINT)len, &bw);
    if (put) *put = (size_t)bw;
    if (fr != FR_OK) { set_err("f_write", fr); return false; }
    return true;
}

bool storage_sync(storage_file_t *f)
{
    FIL *fp = live_fil(f);
    if (!fp) return false;
    FRESULT fr = f_sync(fp);
    if (fr != FR_OK) { set_err("f_sync", fr); return false; }
    return true;
}

bool storage_seek(storage_file_t *f, uint32_t offset)
{
    FIL *fp = live_fil(f);
    if (!fp) return false;
    FRESULT fr = f_lseek(fp, (FSIZE_t)offset);
    if (fr != FR_OK) { set_err("f_lseek", fr); return false; }
    return true;
}

uint32_t storage_tell(storage_file_t *f)
{
    FIL *fp = live_fil(f);
    return fp ? (uint32_t)f_tell(fp) : 0;
}

uint32_t storage_size(storage_file_t *f)
{
    FIL *fp = live_fil(f);
    return fp ? (uint32_t)f_size(fp) : 0;
}

void storage_close(storage_file_t *f)
{
    if (f && f->impl) {
        size_t i = slot_index(f->impl);
        if (i < STORAGE_FILE_SLOTS && file_pool_used[i]) {
            if (!file_pool_orphan[i]) f_close(&file_pool[i]);
            free_file(&file_pool[i]);
        }
        f->impl = NULL;
    }
}

static storage_timestamp_t fatfs_timestamp(uint16_t fdate, uint16_t ftime)
{
    storage_timestamp_t ts = {0};
    if (fdate == 0) return ts;

    uint16_t year = (uint16_t)(1980u + ((fdate >> 9) & 0x7fu));
    uint8_t month = (uint8_t)((fdate >> 5) & 0x0fu);
    uint8_t day = (uint8_t)(fdate & 0x1fu);
    uint8_t hour = (uint8_t)((ftime >> 11) & 0x1fu);
    uint8_t minute = (uint8_t)((ftime >> 5) & 0x3fu);
    uint8_t second = (uint8_t)((ftime & 0x1fu) * 2u);

    if (month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 59u) {
        return ts;
    }

    ts.valid = true;
    ts.year = year;
    ts.month = month;
    ts.day = day;
    ts.hour = hour;
    ts.minute = minute;
    ts.second = second;
    return ts;
}

void storage_format_timestamp(const storage_timestamp_t *ts, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!ts || !ts->valid) {
        snprintf(out, out_len, "-");
        return;
    }
    snprintf(out, out_len, "%04u-%02u-%02u %02u:%02u",
             (unsigned)ts->year,
             (unsigned)ts->month,
             (unsigned)ts->day,
             (unsigned)ts->hour,
             (unsigned)ts->minute);
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
    if (!card_ok()) return false;
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK && fr != FR_NO_FILE) { set_err("f_unlink", fr); return false; }
    if (fr == FR_OK) change_gen++;
    return true;
}

// Shared scratch for the folder helpers below. They never nest or run
// concurrently, and one long-filename FILINFO is ~280 bytes of scarce RAM.
static DIR tree_dir;
static FILINFO tree_fi;

int storage_path_kind(const char *path)
{
    if (!path || !path[0]) return STORAGE_KIND_ERROR;
    if (!card_ok()) return STORAGE_KIND_ERROR;
    FILINFO *fi = &tree_fi;
    FRESULT fr = f_stat(path, fi);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return STORAGE_KIND_MISSING;
    if (fr != FR_OK) { set_err("f_stat", fr); return STORAGE_KIND_ERROR; }
    return (fi->fattrib & AM_DIR) ? STORAGE_KIND_DIR : STORAGE_KIND_FILE;
}

int storage_dir_is_empty(const char *path)
{
    if (!path || !path[0]) return -1;
    if (!card_ok()) return -1;
    FRESULT fr = f_opendir(&tree_dir, path);
    if (fr != FR_OK) { set_err("f_opendir", fr); return -1; }
    fr = f_readdir(&tree_dir, &tree_fi);   // FatFs never returns "." or ".."
    f_closedir(&tree_dir);
    if (fr != FR_OK) { set_err("f_readdir", fr); return -1; }
    return tree_fi.fname[0] == 0 ? 1 : 0;
}

// Iterative and allocation-free: one DIR, one FILINFO and one path buffer,
// whatever the depth. Each step reopens the current folder and takes its
// first entry: a file is deleted, a folder is descended into, and an empty
// folder is deleted and left. Re-reading from the top is O(n^2) in entries
// per folder, which is fine for core collections and keeps RAM flat.
int storage_delete_tree(const char *path, unsigned budget)
{
    static char cur[256];
    DIR *d = &tree_dir;
    FILINFO *fi = &tree_fi;

    if (!path || !path[0] || strcmp(path, "/") == 0) {
        snprintf(last_err, sizeof(last_err), "refusing to delete the root folder");
        return -1;
    }
    size_t base_len = strlen(path);
    if (base_len >= sizeof cur) {
        snprintf(last_err, sizeof(last_err), "path too long");
        return -1;
    }
    memcpy(cur, path, base_len + 1u);

    while (budget) {
        if (!card_ok()) return -1;
        FRESULT fr = f_opendir(d, cur);
        if (fr != FR_OK) { set_err("f_opendir", fr); return -1; }
        fr = f_readdir(d, fi);
        f_closedir(d);
        if (fr != FR_OK) { set_err("f_readdir", fr); return -1; }

        size_t n = strlen(cur);
        if (fi->fname[0] == 0) {
            // Current folder is empty: remove it and step back up
            fr = f_unlink(cur);
            if (fr != FR_OK) { set_err("f_unlink", fr); return -1; }
            change_gen++;
            budget--;
            if (n <= base_len) return 1;          // that was the target itself
            char *slash = strrchr(cur, '/');
            if (!slash) return 1;
            *slash = 0;
            continue;
        }

        size_t name_len = strlen(fi->fname);
        if (n + 1u + name_len >= sizeof cur) {
            snprintf(last_err, sizeof(last_err), "path too long inside %.80s", cur);
            return -1;
        }
        cur[n] = '/';
        memcpy(cur + n + 1u, fi->fname, name_len + 1u);

        if (fi->fattrib & AM_DIR) continue;         // descend; removed once empty

        fr = f_unlink(cur);
        if (fr != FR_OK) {
            // e.g. FR_DENIED for a read-only file: say which one
            char err_path[64];
            snprintf(err_path, sizeof err_path, "%s", cur);
            set_err("f_unlink", fr);
            size_t el = strlen(last_err);
            snprintf(last_err + el, sizeof(last_err) - el, " (%s)", err_path);
            return -1;
        }
        cur[n] = 0;
        change_gen++;
        budget--;
    }
    return 0;   // more remain; call again
}

bool storage_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return false;
    if (!card_ok()) return false;
    FRESULT fr = f_rename(old_path, new_path);
    if (fr != FR_OK) { set_err("f_rename", fr); return false; }
    change_gen++;
    return true;
}

bool storage_mkdir(const char *path)
{
    if (!path || !path[0]) return false;
    if (!card_ok()) return false;
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK && fr != FR_EXIST) { set_err("f_mkdir", fr); return false; }
    if (fr == FR_OK) change_gen++;
    return true;
}

// Walks an open directory, calling cb for each entry that passes the filter.
// Closes the directory when the walk ends or fails; leaves it open when the
// callback stops it (lrFull) so a resumable listing can carry on later.
static list_result_t list_walk(DIR *dir, const char *p, storage_list_cb_t cb, void *ctx, bool cores_only)
{
    FILINFO fi;
    bool in_theme_dir = ext_equal_ci_local(p, M65_THEME_DIR_PATH) ||
                        (p[0] == '/' && ext_equal_ci_local(p + 1, M65_THEME_DIR_PATH));
    for (;;) {
        FRESULT fr = f_readdir(dir, &fi);
        if (fr != FR_OK) { set_err("f_readdir", fr); f_closedir(dir); return lrFailed; }
        if (fi.fname[0] == 0) break;
        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (!cores_only || is_dir || has_core_ext(fi.fname) ||
            (in_theme_dir && has_theme_ext(fi.fname)) ||
            has_partial_core_ext(fi.fname)) {
            storage_timestamp_t modified = fatfs_timestamp(fi.fdate, fi.ftime);
            if (!cb(fi.fname, (uint32_t)fi.fsize, is_dir, &modified, ctx)) {
                return lrFull;
            }
        }
    }
    f_closedir(dir);
    return lrSuccess;
}

static list_result_t list_once(const char *path, storage_list_cb_t cb, void *ctx, bool cores_only)
{
    DIR dir;
    const char *p = (path && path[0]) ? path : "/";
    if (!card_ok()) return lrFailed;
    FRESULT fr = f_opendir(&dir, p);
    if (fr != FR_OK) { set_err("f_opendir", fr); return lrFailed; }
    list_result_t r = list_walk(&dir, p, cb, ctx, cores_only);
    if (r == lrFull) f_closedir(&dir);   // stopped early: nothing will resume it
    return r;
}

list_result_t storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx)
{
    return list_once(path, cb, ctx, true);
}

list_result_t storage_list_dir(const char *path, storage_list_cb_t cb, void *ctx)
{
    return list_once(path, cb, ctx, false);
}

static DIR resumable_dir;
static bool resumable_open;
static uint32_t resumable_gen;
static char resumable_path[256];

void storage_list_resumable_abort(void)
{
    if (resumable_open) f_closedir(&resumable_dir);
    resumable_open = false;
}

list_result_t storage_list_cores_resumable(const char *path, storage_list_cb_t cb, void *ctx, bool listing_continues)
{
    const char *p = (path && path[0]) ? path : "/";
    if (!listing_continues) {
        storage_list_resumable_abort();
        if (!card_ok()) return lrFailed;
        FRESULT fr = f_opendir(&resumable_dir, p);
        if (fr != FR_OK) { set_err("f_opendir", fr); return lrFailed; }
        resumable_open = true;
        resumable_gen = mount_gen;
        snprintf(resumable_path, sizeof resumable_path, "%s", p);
    } else {
        if (!resumable_open) {
            snprintf(last_err, sizeof(last_err), "no listing in progress");
            return lrFailed;
        }
        if (!card_ok() || resumable_gen != mount_gen) {
            // The volume this listing was walking is gone (card swapped/remounted).
            resumable_open = false;
            snprintf(last_err, sizeof(last_err), "sd card removed");
            return lrFailed;
        }
    }
    list_result_t r = list_walk(&resumable_dir, resumable_path, cb, ctx, true);
    if (r != lrFull) resumable_open = false;   // walked to the end, or failed: closed
    return r;
}
