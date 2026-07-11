#include "storage.h"

#include <stdio.h>

static const char *err = "storage disabled: build with USE_FATFS=1";

bool storage_mount(void) { return false; }
void storage_unmount(void) { }
const char *storage_last_error(void) { return err; }

bool storage_open(storage_file_t *f, const char *path) { (void)f; (void)path; return false; }
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got) { (void)f; (void)buf; (void)len; if (got) *got = 0; return false; }
bool storage_seek(storage_file_t *f, uint32_t offset) { (void)f; (void)offset; return false; }
uint32_t storage_tell(storage_file_t *f) { (void)f; return 0; }
uint32_t storage_size(storage_file_t *f) { (void)f; return 0; }
void storage_close(storage_file_t *f) { if (f) f->impl = NULL; }
bool storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx) { (void)path; (void)cb; (void)ctx; return false; }
