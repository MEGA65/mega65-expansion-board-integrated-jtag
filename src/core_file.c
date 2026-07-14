#include "core_file.h"

#include <stdio.h>
#include <string.h>

static char last_err[128];

static void set_err(const char *msg)
{
    snprintf(last_err, sizeof(last_err), "%s", msg);
}

const char *core_last_error(void)
{
    return last_err[0] ? last_err : "no core-file error";
}

static bool read_exact(storage_file_t *f, void *buf, size_t len)
{
    size_t got = 0;
    if (!storage_read(f, buf, len, &got)) return false;
    return got == len;
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void copy_fixed_string(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t n = 0;
    if (!dst_len) return;
    while (n + 1 < dst_len && n < src_len && src[n]) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = 0;
}

static bool skip_bytes(storage_file_t *f, uint32_t n)
{
    uint32_t pos = storage_tell(f);
    return storage_seek(f, pos + n);
}

static bool parse_cor_header(core_file_t *cf)
{
    uint8_t hdr[128];
    if (!storage_seek(&cf->file, 0) || !read_exact(&cf->file, hdr, sizeof hdr)) {
        set_err("cannot read COR header");
        return false;
    }
    copy_fixed_string(cf->title, sizeof cf->title, hdr + 0x10, 32);
    copy_fixed_string(cf->version, sizeof cf->version, hdr + 0x30, 32);
    copy_fixed_string(cf->model, sizeof cf->model, hdr + 0x50, 32);
    cf->model_id = hdr[0x70];
    return true;
}

static bool parse_m65j(core_file_t *cf)
{
    uint8_t hdr[16];
    if (!storage_seek(&cf->file, 0) || !read_exact(&cf->file, hdr, sizeof hdr)) {
        set_err("cannot read M65J header");
        return false;
    }
    if (memcmp(hdr, "M65J", 4) != 0) {
        set_err("bad M65J magic");
        return false;
    }
    cf->kind = CORE_KIND_RAW_M65J;
    cf->payload_length = be32(hdr + 4);
    cf->expected_idcode = be32(hdr + 8);
    cf->payload_offset = 16;
    return true;
}

static bool parse_xilinx_bit_at(core_file_t *cf, uint32_t base)
{
    uint8_t b[8];
    if (!storage_seek(&cf->file, base) || !read_exact(&cf->file, b, 2)) {
        set_err("cannot read .bit header length");
        return false;
    }

    uint16_t n = be16(b);
    if (n == 0 || n > 4096) {
        set_err("not a recognised Xilinx .bit wrapper");
        return false;
    }
    if (!skip_bytes(&cf->file, n)) {
        set_err("cannot skip .bit preamble");
        return false;
    }

    // Xilinx .bit files normally have:
    //   uint16 magic_len
    //   magic bytes
    //   uint16 0x0001
    //   'a' uint16 design_name_len design_name
    //   'b' uint16 part_len        part
    //   'c' uint16 date_len        date
    //   'd' uint16 time_len        time
    //   'e' uint32 payload_len     raw config payload
    //
    // Do NOT treat the 0x0001 marker as a length field. Doing so skips
    // the following 'a' tag and makes the next byte appear to be tag 0x00.
    if (!read_exact(&cf->file, b, 2)) {
        set_err("cannot read .bit marker");
        return false;
    }
    uint16_t marker = be16(b);
    if (marker != 1) {
        set_err("bad .bit marker after magic");
        return false;
    }

    for (unsigned rec = 0; rec < 32; rec++) {
        uint8_t tag;
        if (!read_exact(&cf->file, &tag, 1)) {
            set_err("unexpected end of .bit records");
            return false;
        }
        if (tag >= 'a' && tag <= 'd') {
            if (!read_exact(&cf->file, b, 2)) {
                set_err("cannot read .bit string length");
                return false;
            }
            uint16_t slen = be16(b);
            if (!skip_bytes(&cf->file, slen)) {
                set_err("cannot skip .bit string field");
                return false;
            }
        } else if (tag == 'e') {
            if (!read_exact(&cf->file, b, 4)) {
                set_err("cannot read .bit payload length");
                return false;
            }
            cf->payload_length = be32(b);
            cf->payload_offset = storage_tell(&cf->file);
            cf->expected_idcode = 0;

            // m65/fpgajtag derives the IDCODE from payload+0x80. Keep it optional.
            if (cf->payload_length > 0x84) {
                uint32_t save = storage_tell(&cf->file);
                if (storage_seek(&cf->file, cf->payload_offset + 0x80) && read_exact(&cf->file, b, 4)) {
                    cf->expected_idcode = be32(b);
                }
                storage_seek(&cf->file, save);
            }
            // 0xffffffff is normally a Xilinx dummy word near the start of
            // the payload, not a useful expected IDCODE. Treat it as unknown.
            if (cf->expected_idcode == 0xffffffffu) cf->expected_idcode = 0;
            return true;
        } else {
            set_err("unknown .bit field tag before payload");
            return false;
        }
    }

    set_err(".bit payload field not found");
    return false;
}


static bool find_xilinx_bit_wrapper(core_file_t *cf, uint32_t start, uint32_t limit, uint32_t *base_out)
{
    // Search for the common Xilinx .bit wrapper preamble:
    // 00 09 0f f0 0f f0 0f f0 0f f0
    static const uint8_t sig[] = { 0x00, 0x09, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0 };
    uint8_t buf[256];
    uint32_t pos = start;
    uint32_t end = start + limit;
    uint32_t file_size = storage_size(&cf->file);
    if (end > file_size) end = file_size;

    while (pos + sizeof(sig) <= end) {
        uint32_t want = end - pos;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (!storage_seek(&cf->file, pos)) return false;
        size_t got = 0;
        if (!storage_read(&cf->file, buf, want, &got) || got == 0) return false;

        for (uint32_t i = 0; i + sizeof(sig) <= got; i++) {
            if (memcmp(buf + i, sig, sizeof(sig)) == 0) {
                *base_out = pos + i;
                return true;
            }
        }

        if (got < sizeof(sig)) break;
        pos += (uint32_t)got - (uint32_t)sizeof(sig) + 1u;
    }

    return false;
}

bool core_open(core_file_t *cf, const char *path)
{
    if (!cf || !path) return false;
    memset(cf, 0, sizeof *cf);
    snprintf(cf->path, sizeof(cf->path), "%s", path);

    if (!storage_open(&cf->file, path)) {
        snprintf(last_err, sizeof(last_err), "open failed: %s", storage_last_error());
        return false;
    }

    uint8_t magic[16];
    if (!storage_seek(&cf->file, 0) || !read_exact(&cf->file, magic, sizeof magic)) {
        set_err("cannot read file header");
        storage_close(&cf->file);
        return false;
    }

    if (memcmp(magic, "M65J", 4) == 0) {
        if (!parse_m65j(cf)) { storage_close(&cf->file); return false; }
    } else if (memcmp(magic, "MEGA65BITSTREAM0", 16) == 0) {
        // MEGA65 .COR files usually have a 4096-byte container header followed
        // by the original Xilinx .bit file. Be a little more tolerant and scan
        // for the .bit wrapper if it is not exactly there.
        cf->kind = CORE_KIND_COR;
        if (!parse_cor_header(cf)) { storage_close(&cf->file); return false; }
        if (!parse_xilinx_bit_at(cf, 4096)) {
            uint32_t bit_base = 0;
            if (!find_xilinx_bit_wrapper(cf, 0, 1024u * 1024u, &bit_base) ||
                !parse_xilinx_bit_at(cf, bit_base)) {
                storage_close(&cf->file);
                return false;
            }
        }
    } else {
        cf->kind = CORE_KIND_BIT;
        if (!parse_xilinx_bit_at(cf, 0)) { storage_close(&cf->file); return false; }
    }

    last_err[0] = 0;
    return true;
}

void core_close(core_file_t *cf)
{
    if (cf) storage_close(&cf->file);
}

bool core_rewind_payload(core_file_t *cf)
{
    return cf && storage_seek(&cf->file, cf->payload_offset);
}

bool core_read_payload(core_file_t *cf, uint8_t *buf, size_t len, size_t *got)
{
    if (!cf || !buf) return false;
    return storage_read(&cf->file, buf, len, got);
}

const char *core_kind_name(core_kind_t kind)
{
    switch (kind) {
    case CORE_KIND_BIT: return "BIT";
    case CORE_KIND_COR: return "COR";
    case CORE_KIND_RAW_M65J: return "M65J";
    default: return "UNKNOWN";
    }
}
