#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_file.h"

uint8_t core_board_hint_from_name(const char *path);
bool core_matches_board(const core_file_t *cf, const char *path, uint8_t board_rev);
const char *core_board_label(uint8_t board_rev);

