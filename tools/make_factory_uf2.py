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


def add_region(regions: list[tuple[int, bytes, str]], addr: int, data: bytes, name: str) -> None:
    if not data:
        raise SystemExit(f"{name}: empty input")
    end = addr + len(data)
    for other_addr, other_data, other_name in regions:
        other_end = other_addr + len(other_data)
        if addr < other_end and other_addr < end:
            raise SystemExit(
                f"{name} at 0x{addr:08x}..0x{end:08x} overlaps "
                f"{other_name} at 0x{other_addr:08x}..0x{other_end:08x}"
            )
    regions.append((addr, data, name))


def iter_blocks(regions: list[tuple[int, bytes, str]]) -> list[tuple[int, bytes]]:
    blocks: list[tuple[int, bytes]] = []
    for base, data, _name in sorted(regions):
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

    regions: list[tuple[int, bytes, str]] = []
    add_region(regions, XIP_BASE + args.bootloader_offset, bootloader, "bootloader")
    add_region(regions, XIP_BASE + args.app_offset, app, "app")
    blocks = iter_blocks(regions)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_uf2(args.output, blocks, UF2_FAMILY_RP2040)
    print(
        f"Wrote {args.output}: bootloader={len(bootloader)} bytes "
        f"app={len(app)} bytes blocks={len(blocks)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
