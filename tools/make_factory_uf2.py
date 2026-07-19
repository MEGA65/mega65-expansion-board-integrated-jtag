#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
from pathlib import Path


UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_FAMILY_RP2040 = 0xE48BFF56
XIP_BASE = 0x10000000
BLOCK_PAYLOAD = 256


def parse_int(s: str) -> int:
    return int(s, 0)


def iter_blocks(regions: list[tuple[int, bytes]]) -> list[tuple[int, bytes]]:
    blocks: list[tuple[int, bytes]] = []
    for base, data in sorted(regions):
        for off in range(0, len(data), BLOCK_PAYLOAD):
            chunk = data[off:off + BLOCK_PAYLOAD]
            blocks.append((base + off, chunk))
    return blocks


def write_uf2(path: Path, blocks: list[tuple[int, bytes]], family: int) -> None:
    with path.open("wb") as f:
        total = len(blocks)
        for block_no, (addr, chunk) in enumerate(blocks):
            payload = chunk.ljust(BLOCK_PAYLOAD, b"\xff")
            header = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                UF2_FLAG_FAMILY_ID_PRESENT,
                addr,
                BLOCK_PAYLOAD,
                block_no,
                total,
                family,
            )
            f.write(header)
            f.write(payload)
            f.write(b"\x00" * (512 - len(header) - BLOCK_PAYLOAD - 4))
            f.write(struct.pack("<I", UF2_MAGIC_END))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Pack a resident bootloader and relocated app into one RP2040 UF2")
    ap.add_argument("--bootloader", required=True, type=Path, help="bootloader .bin linked at XIP base")
    ap.add_argument("--app", required=True, type=Path, help="relocated application .bin")
    ap.add_argument("--output", required=True, type=Path, help="factory UF2 to write")
    ap.add_argument("--bootloader-offset", default="0x0", type=parse_int)
    ap.add_argument("--bootloader-size", default="0x10000", type=parse_int)
    ap.add_argument("--app-offset", default="0x10000", type=parse_int)
    ap.add_argument("--app-size", default="0xF0000", type=parse_int)
    args = ap.parse_args(argv)

    bootloader = args.bootloader.read_bytes()
    app = args.app.read_bytes()
    if len(bootloader) > args.bootloader_size:
        raise SystemExit(
            f"bootloader is {len(bootloader)} bytes, exceeds reserved {args.bootloader_size} bytes"
        )
    if len(app) > args.app_size:
        raise SystemExit(f"app is {len(app)} bytes, exceeds slot size {args.app_size} bytes")

    if args.bootloader_offset != 0:
        raise SystemExit("factory UF2 currently expects bootloader-offset=0")
    if args.app_offset < len(bootloader):
        raise SystemExit(
            f"app offset 0x{args.app_offset:x} overlaps bootloader length 0x{len(bootloader):x}"
        )

    # Keep this UF2 contiguous from flash base through the app. The RP2040 ROM
    # UF2 loader should handle sparse address ranges, but contiguous blocks are
    # easier to reason about and avoid host/bootloader edge cases during the
    # first factory install.
    image = bytearray(args.app_offset + len(app))
    image[:] = b"\xff" * len(image)
    image[:len(bootloader)] = bootloader
    image[args.app_offset:args.app_offset + len(app)] = app

    regions: list[tuple[int, bytes]] = [(XIP_BASE, bytes(image))]
    blocks = iter_blocks(regions)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_uf2(args.output, blocks, UF2_FAMILY_RP2040)
    print(
        f"Wrote {args.output}: bootloader={len(bootloader)} bytes "
        f"pad={args.app_offset - len(bootloader)} bytes app={len(app)} bytes blocks={len(blocks)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
