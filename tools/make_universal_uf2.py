#!/usr/bin/env python3
"""Combine per-chip factory UF2s into one universal UF2.

UF2 blocks carry a chip family ID, and block numbering runs separately for
each family. Each chip's USB (BOOTSEL) bootloader writes only the blocks for
its own family and ignores the rest, and so does the OTA updater
(src/ota_apply.c). Concatenating complete single-family UF2s therefore gives
one file that installs on every chip it contains.

The inputs are written in the order given. `make universal` puts the RP2350
image first, so an RP2350 sees its own blocks first, as it would in its own
factory UF2.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_NOT_MAIN_FLASH = 0x00000001
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_FAMILY_ABSOLUTE = 0xE48BFF57
FAMILY_NAMES = {
    0xE48BFF56: "RP2040",
    0xE48BFF59: "RP2350-ARM-S",
    0xE48BFF5A: "RP2350-RISCV",
    0xE48BFF5B: "RP2350-ARM-NS",
}


def check_single_family(path: Path) -> tuple[int, int, bytes]:
    """Returns (family, block count, file bytes) for a complete one-chip UF2."""
    data = path.read_bytes()
    if not data or len(data) % 512:
        raise SystemExit(f"{path}: not a UF2 file (size {len(data)} is not a multiple of 512)")
    family = None
    count = 0
    num_blocks = None
    for i in range(0, len(data), 512):
        m0, m1, flags, _addr, _size, block_no, n_blocks, fam = struct.unpack_from("<8I", data, i)
        (m_end,) = struct.unpack_from("<I", data, i + 508)
        if (m0, m1, m_end) != (UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END):
            raise SystemExit(f"{path}: bad UF2 block {i // 512}")
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            continue
        if not flags & UF2_FLAG_FAMILY_ID_PRESENT:
            raise SystemExit(f"{path}: block {i // 512} has no family ID; a universal UF2 needs one on every block")
        if fam == UF2_FAMILY_ABSOLUTE:
            continue
        if family is None:
            family, num_blocks = fam, n_blocks
        if fam != family:
            raise SystemExit(f"{path}: contains more than one chip family already")
        if block_no != count or n_blocks != num_blocks:
            raise SystemExit(f"{path}: blocks out of order or missing at block {i // 512}")
        count += 1
    if family is None or count != num_blocks:
        raise SystemExit(f"{path}: incomplete UF2 ({count} of {num_blocks} blocks)")
    return family, count, data


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Combine per-chip factory UF2s into one universal UF2")
    ap.add_argument("--output", required=True, type=Path, help="universal UF2 to write")
    ap.add_argument("inputs", nargs="+", type=Path, help="single-chip factory UF2s, in output order")
    args = ap.parse_args(argv)
    if len(args.inputs) < 2:
        raise SystemExit("need at least two UF2 files to combine")

    seen: dict[int, Path] = {}
    parts: list[bytes] = []
    summary: list[str] = []
    for path in args.inputs:
        family, count, data = check_single_family(path)
        if family in seen:
            raise SystemExit(f"{path} and {seen[family]} are both for {FAMILY_NAMES.get(family, hex(family))}")
        seen[family] = path
        parts.append(data)
        summary.append(f"{FAMILY_NAMES.get(family, hex(family))}={count} blocks")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(b"".join(parts))
    print(f"Wrote {args.output}: {', '.join(summary)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
