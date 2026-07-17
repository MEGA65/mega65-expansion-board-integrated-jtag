#include "machine_identity.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef M65_USB_PRODUCT_BASE
#define M65_USB_PRODUCT_BASE "MEGA65 Expansion Board Integrated JTAG"
#endif

static char machine_name[M65_MACHINE_NAME_MAX + 1u];
static uint8_t machine_board_rev;

void machine_identity_init_defaults(void)
{
    machine_name[0] = 0;
    machine_board_rev = 0;
}

bool machine_identity_valid_name(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n > M65_MACHINE_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

void machine_identity_set_name(const char *name)
{
    if (!name || !name[0]) {
        machine_name[0] = 0;
        return;
    }
    snprintf(machine_name, sizeof machine_name, "%s", name);
}

const char *machine_identity_name(void)
{
    return machine_name[0] ? machine_name : "";
}

void machine_identity_set_board_rev(uint8_t board_rev)
{
    machine_board_rev = (board_rev == 3u || board_rev == 6u) ? board_rev : 0u;
}

uint8_t machine_identity_board_rev(void)
{
    return machine_board_rev;
}

const char *machine_identity_board_token(void)
{
    switch (machine_board_rev) {
    case 3: return "r3";
    case 6: return "r6";
    default: return "r0";
    }
}

void machine_identity_format(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s:%s",
             machine_identity_board_token(),
             machine_name[0] ? machine_name : "unnamed");
}

const char *machine_identity_usb_product(void)
{
    static char product[64];
    if (machine_name[0]) {
        snprintf(product, sizeof product, "%s %s", M65_USB_PRODUCT_BASE, machine_name);
    } else {
        snprintf(product, sizeof product, "%s", M65_USB_PRODUCT_BASE);
    }
    return product;
}
