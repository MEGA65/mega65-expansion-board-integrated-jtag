#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *impl;
} storage_file_t;

typedef void (*storage_list_cb_t)(const char *name, uint32_t size, bool is_dir, void *ctx);

bool storage_mount(void);
void storage_unmount(void);
const char *storage_last_error(void);

bool storage_open(storage_file_t *f, const char *path);
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got);
bool storage_seek(storage_file_t *f, uint32_t offset);
uint32_t storage_tell(storage_file_t *f);
uint32_t storage_size(storage_file_t *f);
void storage_close(storage_file_t *f);

bool storage_list_cores(const char *path, storage_list_cb_t cb, void *ctx);
