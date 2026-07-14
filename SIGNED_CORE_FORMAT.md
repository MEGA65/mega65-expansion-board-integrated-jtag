# Signed core file trailer

This firmware supports a signed upload/fetch container for `.bit`, `.cor`, and
`.m65j` files. The signature is a fixed 256-byte trailer appended to the bytes
sent over HTTP or fetched from a URL. The firmware hashes the payload while it is
receiving it, buffers the final trailer, verifies the signature, and writes only
the payload bytes to the final SD-card file.

The trailer is not required to be aligned. It is simply the final 256 bytes of a
transfer. Because of this, signed HTTP PUT and firmware-side fetches require a
valid `Content-Length`.

## Policy

`REMOTE_ENABLE.cfg` controls whether signatures are required:

```ini
require_signatures=1
trusted_key=p256:04<64-byte-X-and-Y-public-key-as-hex>
```

If `require_signatures=1`, uploads and URL fetches must end with a valid trailer
signed by one of the configured `trusted_key` entries. If the trailer is absent
or invalid, the temporary file is deleted.

If `require_signatures=0`, unsigned files are accepted. If a transfer does carry
a trailer magic, the firmware still verifies it and rejects the file if the
signature is invalid.

## Version 1 trailer

All integers are little-endian.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 32 | Magic: `M65JTAG-SIGBLOCK-V1` plus fixed binary bytes |
| 32 | 2 | Version, currently `1` |
| 34 | 2 | Header length, currently `256` |
| 36 | 4 | Flags, currently `0` |
| 40 | 4 | Payload length in bytes, excluding this trailer |
| 44 | 1 | File type: `0=any`, `1=bit`, `2=cor`, `3=m65j` |
| 45 | 1 | Board ID: `0=any`, `3=R3`, `6=R6` |
| 46 | 1 | Hash algorithm: `1=SHA-256` |
| 47 | 1 | Signature algorithm: `1=ECDSA-P256-SHA256` |
| 48 | 16 | Key ID: first 16 bytes of SHA-256 over the raw public key, or all zero to try all keys |
| 64 | 32 | SHA-256 over the payload bytes |
| 96 | 96 | Destination filename, NUL-terminated; blank disables filename checking |
| 192 | 64 | Raw ECDSA signature `r || s`, 32 bytes each |

The signature is calculated over `SHA-256(trailer[0:192])`. That signed metadata
includes the payload hash, payload length, file type, board ID, key ID, filename,
and algorithm identifiers.

If the filename field is non-empty, firmware compares it with the basename of
the destination path. For example, `/files/game.bit` and `DOWNLOADS/game.bit`
both check against `game.bit`.

## Notes

- v1 uses ECDSA P-256 because the Pico SDK already provides a maintained Mbed
  TLS implementation for it.
- The trailer is algorithm-labelled so a later version can add Ed25519 without
  changing the receive flow.
- The payload stored on SD is the original file content without the 256-byte
  trailer.
- Direct stream-to-JTAG cannot safely verify until the trailer has arrived. When
  signatures are required, `PUT /jtag` therefore spools to `DOWNLOADS/`, verifies
  the trailer, and only then programs the verified core file.

## Host signing utility

List local public keys and the matching config lines:

```sh
tools/bless-core.py --keys
```

If the selected private key does not exist, `bless-core.py` prompts to create it
with OpenSSL and asks for a key name, defaulting to `default`.

Print the selected key's `REMOTE_ENABLE.cfg` entry:

```sh
tools/bless-core.py --print-trusted-key core.bit
```

Create a signed transfer:

```sh
tools/bless-core.py --board 6 --bless -o core.signed.bit core.bit
```

Upload in one command:

```sh
tools/bless-core.py --board 6 \
  --put http://mega65-jtag.local/files/core.bit core.bit
```

Upload and JTAG in one command:

```sh
tools/bless-core.py --board 6 \
  --device http://mega65-jtag.local core.bit
```

If the input already has a signature trailer, `bless-core.py` prints an `INFO:`
message and uploads or copies it unchanged instead of appending another trailer.
