# MEGA65 Expansion Board Integrated JTAG Signed Core Trailer

**Status:** v0.1 experimental. This signed-file container, trailer layout,
signature policy, key configuration, HTTP upload behaviour, and host utility
commands are all still subject to change.

The MEGA65 Expansion Board Integrated JTAG firmware currently supports a signed
upload/fetch container for `.bit`, `.cor`, and `.m65j` files. The signature is a
fixed 256-byte trailer appended to the bytes sent over HTTP or fetched from a
URL. The firmware hashes the payload while it is receiving it, buffers the final
trailer, verifies the signature, and writes only the payload bytes to the final
SD-card file.

The trailer is not required to be aligned. It is simply the final 256 bytes of a
transfer. Because of this, signed HTTP PUT and firmware-side fetches require a
valid `Content-Length`.

## Policy

`mega65-jtag.cfg` controls whether signatures are required:

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

## Mirror manifests

Autofetch requires the channel manifest itself to be signed, regardless of the
general `require_signatures` setting. The signed transfer is stored on SD as the
manifest payload after the 256-byte trailer has been verified and stripped.

Manifest lines use v2 format:

```text
<payload-sha256> <transfer-sha256>  <relative-filename>
```

`payload-sha256` hashes the final SD-card file after the signature trailer has
been removed. `transfer-sha256` hashes the exact HTTP object, including its
signature trailer. The firmware rejects old one-hash manifests, unsigned
manifests, bad signatures, transfer hash mismatches, payload hash mismatches,
and manifest entries outside normal core file paths.

## Host signing utility

List local public keys and the matching config lines:

```sh
tools/m65j.py keys
```

If the selected private key does not exist, the client prompts to create it with
OpenSSL and asks for a key name, defaulting to `default`.

Print the selected key's `mega65-jtag.cfg` entry:

```sh
tools/m65j.py bless --print-trusted-key core.bit
```

Create a signed transfer:

```sh
tools/m65j.py bless --board 6 -o core.signed.bit core.bit
```

Upload in one command:

```sh
tools/m65j.py put http://mega65-jtag.local/files/core.bit \
  core.bit --board 6
```

Upload and JTAG in one command:

```sh
tools/m65j.py push http://mega65-jtag.local core.bit --board 6
```

If the input already has a signature trailer, the client prints an `INFO:`
message and uploads or copies it unchanged instead of appending another trailer.
