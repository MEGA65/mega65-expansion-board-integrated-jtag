#include "storage.h"

#include <string.h>

static char last_err[96] = "storage disabled; build without nofatfs to enable SD/FatFs";

bool storage_mount(void) { return false; }
void storage_unmount(void) {}
bool storage_is_mounted(void) { return false; }
const char *storage_last_error(void) { return last_err; }
bool storage_open(storage_file_t *f, const char *path) { (void)f; (void)path; return false; }
bool storage_open_write(storage_file_t *f, const char *path, bool truncate) { (void)f; (void)path; (void)truncate; return false; }
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got) { (void)f; (void)buf; (void)len; if (got) *got = 0; return false; }
bool storage_write(storage_file_t *f, const void *buf, size_t len, size_t *put) { (void)f; (void)buf; (void)len; if (put) *put = 0; return false; }
bool storage_sync(storage_file_t *f) { (void)f; return false; }
bool storage_seek(storage_file_t *f, uint32_t offset) { (void)f; (void)offset; return false; }
uint32_t storage_tell(storage_file_t *f) { (void)f; return 0; }
uint32_t storage_size(storage_file_t *f) { (void)f; return 0; }
void storage_close(storage_file_t *f) { if (f) f->impl = NULL; }
bool storage_delete(const char *path) { (void)path; return false; }
bool storage_rename(const char *old_path, const char *new_path) { (void)old_path; (void)new_path; return false; }
bool storage_mkdir(const char *path) { (void)path; return false; }
bool storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx) { (void)path; (void)cb; (void)ctx; return false; }
void storage_sd_probe(void) {}
bool storage_sd_may_mount(void) { return false; }
bool storage_sd_set_transport(const char *name) { (void)name; return false; }
bool storage_sd_transport_locked(void) { return true; }
const char *storage_sd_transport_name(void) { return "disabled"; }
