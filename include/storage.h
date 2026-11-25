#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *impl;
} storage_file_t;

typedef struct {
    bool valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} storage_timestamp_t;

typedef enum {
    lrFailed,   // List encountered an error
    lrSuccess,  // List succeeded and is finished
    lrFull,     // List filled the available buffer
} list_result_t;

typedef bool (*storage_list_cb_t)(const char *name,
                                  uint32_t size,
                                  bool is_dir,
                                  const storage_timestamp_t *modified,
                                  void *ctx);

bool storage_mount(void);
void storage_unmount(void);
bool storage_is_mounted(void);
const char *storage_last_error(void);

bool storage_open(storage_file_t *f, const char *path);
bool storage_open_write(storage_file_t *f, const char *path, bool truncate);
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got);
bool storage_write(storage_file_t *f, const void *buf, size_t len, size_t *put);
bool storage_sync(storage_file_t *f);
bool storage_seek(storage_file_t *f, uint32_t offset);
uint32_t storage_tell(storage_file_t *f);
uint32_t storage_size(storage_file_t *f);
void storage_close(storage_file_t *f);

bool storage_delete(const char *path);
bool storage_rename(const char *old_path, const char *new_path);
bool storage_mkdir(const char *path);

enum {
    STORAGE_KIND_ERROR = -1,
    STORAGE_KIND_MISSING = 0,
    STORAGE_KIND_FILE = 1,
    STORAGE_KIND_DIR = 2,
};
// One of STORAGE_KIND_*
int storage_path_kind(const char *path);
// 1 = empty, 0 = has entries, -1 = error
int storage_dir_is_empty(const char *path);
// Deletes the folder at path and everything inside it, removing at most
// `budget` entries per call so no single call blocks for long.
// Returns 1 = finished, 0 = more remain (call again), -1 = error.
int storage_delete_tree(const char *path, unsigned budget);
// One-shot listings: each call opens, walks and closes its own directory
// handle, so listings can nest (callbacks may list subfolders) and can never
// disturb each other or the resumable listing below. A callback returning
// false stops the walk (lrFull).
list_result_t storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx);
list_result_t storage_list_dir(const char *path, storage_list_cb_t cb, void *ctx);
// The one resumable listing (chunked AT+CORELIST / AT+COREDETAIL output): it
// keeps its own handle open between calls while the callback returns false
// (lrFull), and listing_continues=true carries on from there. Starting a new
// one (listing_continues=false) abandons any unfinished one.
list_result_t storage_list_cores_resumable(const char *path, storage_list_cb_t cb, void *ctx, bool listing_continues);
void storage_list_resumable_abort(void);
void storage_format_timestamp(const storage_timestamp_t *ts, char *out, size_t out_len);

typedef enum {
    STORAGE_SD_EVENT_NONE = 0,
    STORAGE_SD_EVENT_REMOVED,   // card pulled; volume unmounted, open handles invalidated
    STORAGE_SD_EVENT_INSERTED,  // card-detect settled to present; caller should mount
} storage_sd_event_t;

// True if a card is in the socket (always true when card detect is disabled).
bool storage_sd_card_present(void);
// Changes whenever the card is mounted, removed or unmounted, and whenever a
// file or folder on it is created, deleted or renamed. Lets clients (the web
// UI, the MEGA65 core loader) notice that a listing they hold may be stale.
uint32_t storage_media_generation(void);
// Debounced card-detect poll; call often from the main loop.
storage_sd_event_t storage_sd_hotplug_poll(void);

void storage_sd_probe(void);
bool storage_sd_may_mount(void);
bool storage_sd_set_transport(const char *name);
bool storage_sd_transport_locked(void);
const char *storage_sd_transport_name(void);
