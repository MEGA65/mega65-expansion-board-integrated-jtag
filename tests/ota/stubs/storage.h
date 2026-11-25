#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { FILE *fp; } storage_file_t;
bool storage_open(storage_file_t *f, const char *path);
bool storage_read(storage_file_t *f, void *buf, size_t len, size_t *got);
bool storage_seek(storage_file_t *f, uint32_t offset);
void storage_close(storage_file_t *f);
uint32_t storage_size(storage_file_t *f);
