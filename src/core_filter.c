#include "core_filter.h"

#include <ctype.h>
#include <string.h>

static bool token_match(const char *s, char r)
{
    if (!s) return false;
    for (size_t i = 0; s[i]; i++) {
        if (tolower((unsigned char)s[i]) != 'r') continue;
        if (tolower((unsigned char)s[i + 1]) != (unsigned char)r) continue;
        char before = i == 0 ? 0 : s[i - 1];
        char after = s[i + 2];
        bool before_ok = i == 0 || !isalnum((unsigned char)before);
        bool after_ok = after == 0 || !isalnum((unsigned char)after);
        if (before_ok && after_ok) return true;
    }
    return false;
}

static bool contains_model(const char *s, uint8_t board)
{
    if (!s || !*s) return false;
    if (board == 3) return token_match(s, '3') || strstr(s, "mega65r3") || strstr(s, "MEGA65R3");
    if (board == 6) return token_match(s, '6') || strstr(s, "mega65r6") || strstr(s, "MEGA65R6");
    return false;
}

uint8_t core_board_hint_from_name(const char *path)
{
    bool r3 = contains_model(path, 3);
    bool r6 = contains_model(path, 6);
    if (r3 && !r6) return 3;
    if (r6 && !r3) return 6;
    return 0;
}

bool core_matches_board(const core_file_t *cf, const char *path, uint8_t board_rev)
{
    if (board_rev != 3 && board_rev != 6) return true;
    if (cf) {
        if (cf->model_id == 3 || cf->model_id == 6) return cf->model_id == board_rev;
        if (contains_model(cf->model, 3) || contains_model(cf->model, 6)) {
            return contains_model(cf->model, board_rev);
        }
    }
    uint8_t hint = core_board_hint_from_name(path);
    return hint == 0 || hint == board_rev;
}

const char *core_board_label(uint8_t board_rev)
{
    switch (board_rev) {
    case 3: return "R3";
    case 6: return "R6";
    default: return "ANY";
    }
}

