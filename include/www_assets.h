#pragma once

// Built-in copies of the default web interface (sdcard/WWW), embedded at
// build time by tools/embed_www.py. Served when the SD card has no copy.

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;       // file name inside WWW/, e.g. "app.js"
    const uint8_t *data;    // followed by a NUL (not counted in size)
    uint32_t size;
} m65_www_asset_t;

extern const m65_www_asset_t m65_www_assets[];
extern const unsigned m65_www_asset_count;
