#!/usr/bin/env python3
"""Bless, upload, or JTAG-push a MEGA65 JTAG core file.

By default, if --device or --put is supplied, this signs the input and uploads it
in one step. Use --bless to just create a signed local transfer file.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path


MAGIC = bytes([
    ord("M"), ord("6"), ord("5"), ord("J"), ord("T"), ord("A"), ord("G"), ord("-"),
    ord("S"), ord("I"), ord("G"), ord("B"), ord("L"), ord("O"), ord("C"), ord("K"),
    ord("-"), ord("V"), ord("1"), 0xA5, 0x65, 0x19, 0x83, 0x42,
    0x7C, 0xD1, 0x5E, 0x09, 0xBA, 0x6F, 0x34, 0xC8,
])
TRAILER_LEN = 256
SIG_OFF = 192
FILENAME_OFF = 96
FILENAME_LEN = 96
DEFAULT_KEY = Path.home() / ".m65jtag-core-signing.pem"
KEY_DIR = Path.home() / ".m65jtag" / "keys"
DEFAULT_KEY_NAME = "default"


def run_openssl(args: list[str], input_data: bytes | None = None) -> bytes:
    try:
        p = subprocess.run(
            ["openssl", *args],
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
    except FileNotFoundError:
        raise SystemExit("openssl not found in PATH")
    except subprocess.CalledProcessError as e:
        raise SystemExit(e.stderr.decode("utf-8", "replace").strip() or "openssl failed")
    return p.stdout


def key_path_for_name(name: str) -> Path:
    safe = "".join(c for c in name if c.isalnum() or c in ("-", "_", ".")).strip(".")
    if not safe:
        safe = DEFAULT_KEY_NAME
    return KEY_DIR / f"{safe}.pem"


def resolve_key_path(key: Path | None, key_name: str | None, assume_yes: bool) -> tuple[Path, str]:
    if key is not None:
        name = key.stem or DEFAULT_KEY_NAME
        return key, name
    name = key_name or DEFAULT_KEY_NAME
    path = key_path_for_name(name)
    if path.exists() or assume_yes:
        return path, name
    if not sys.stdin.isatty():
        return path, name
    entered = input(f"Name for new signing key [{name}]: ").strip()
    if entered:
        name = entered
        path = key_path_for_name(name)
    return path, name


def ensure_key(path: Path, name: str, assume_yes: bool) -> None:
    if path.exists():
        return
    answer = "y" if assume_yes else ""
    if not assume_yes:
        if not sys.stdin.isatty():
            raise SystemExit(f"private key not found: {path}; rerun with --yes to create key '{name}'")
        answer = input(f"Private key '{name}' does not exist at {path}. Create it with openssl now? [y/N] ")
    if answer.lower() not in ("y", "yes"):
        raise SystemExit("no private key available")
    path.parent.mkdir(parents=True, exist_ok=True)
    run_openssl(["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(path)])
    try:
        path.chmod(0o600)
    except OSError:
        pass
    print(f"Created private key: {path}", file=sys.stderr)


def der_read_len(data: bytes, pos: int) -> tuple[int, int]:
    if pos >= len(data):
        raise ValueError("short DER length")
    b = data[pos]
    pos += 1
    if b < 0x80:
        return b, pos
    n = b & 0x7F
    if n == 0 or n > 4 or pos + n > len(data):
        raise ValueError("bad DER length")
    v = 0
    for _ in range(n):
        v = (v << 8) | data[pos]
        pos += 1
    return v, pos


def der_expect(data: bytes, pos: int, tag: int) -> tuple[bytes, int]:
    if pos >= len(data) or data[pos] != tag:
        raise ValueError(f"expected DER tag 0x{tag:02x}")
    length, pos = der_read_len(data, pos + 1)
    end = pos + length
    if end > len(data):
        raise ValueError("short DER value")
    return data[pos:end], end


def parse_ecdsa_der_sig(sig: bytes) -> bytes:
    seq, end = der_expect(sig, 0, 0x30)
    if end != len(sig):
        raise ValueError("trailing DER signature bytes")
    r, pos = der_expect(seq, 0, 0x02)
    s, pos = der_expect(seq, pos, 0x02)
    if pos != len(seq):
        raise ValueError("trailing ECDSA sequence bytes")

    def fixed32(v: bytes) -> bytes:
        while len(v) > 0 and v[0] == 0:
            v = v[1:]
        if len(v) > 32:
            raise ValueError("ECDSA integer too large")
        return b"\x00" * (32 - len(v)) + v

    return fixed32(r) + fixed32(s)


def raw_public_key_from_private_key(key_path: Path) -> bytes:
    der = run_openssl(["pkey", "-in", str(key_path), "-pubout", "-outform", "DER"])
    for pos in range(len(der) - 65, -1, -1):
        if der[pos] == 0x04 and pos + 65 <= len(der):
            return der[pos:pos + 65]
    raise ValueError("could not extract raw P-256 public key from openssl DER")


def trusted_key_line(key_path: Path) -> str:
    pub = raw_public_key_from_private_key(key_path)
    return f"trusted_key=p256:{pub.hex()}"


def iter_known_keys() -> list[tuple[str, Path]]:
    keys: list[tuple[str, Path]] = []
    if KEY_DIR.exists():
        for path in sorted(KEY_DIR.glob("*.pem")):
            keys.append((path.stem, path))
    if DEFAULT_KEY.exists() and all(path != DEFAULT_KEY for _, path in keys):
        keys.append(("legacy-default", DEFAULT_KEY))
    return keys


def print_keys() -> int:
    keys = iter_known_keys()
    if not keys:
        print(f"No signing keys found in {KEY_DIR}")
        print("Create one with: tools/uart_client.py bless --yes core.bit")
        return 0
    print("Configured local signing keys:\n")
    for name, path in keys:
        print(f"[{name}] {path}")
        try:
            print(f"  {trusted_key_line(path)}")
        except Exception as e:
            print(f"  ERROR: {e}")
    print("\nTo trust a key, copy its trusted_key= line into REMOTE_ENABLE.cfg on the SD card.")
    print("For enforcement, also set: require_signatures=1")
    return 0


def file_type_for(path: Path) -> int:
    ext = path.suffix.lower()
    if ext == ".bit":
        return 1
    if ext == ".cor":
        return 2
    if ext == ".m65j":
        return 3
    return 0


def default_signed_output(path: Path) -> Path:
    if path.suffix:
        return path.with_name(f"{path.stem}.signed{path.suffix}")
    return path.with_name(f"{path.name}.signed")


def blessed_filename(data: bytes) -> str | None:
    if len(data) < TRAILER_LEN or data[-TRAILER_LEN:-TRAILER_LEN + len(MAGIC)] != MAGIC:
        return None
    raw = data[-TRAILER_LEN + FILENAME_OFF:-TRAILER_LEN + FILENAME_OFF + FILENAME_LEN]
    raw = raw.split(b"\x00", 1)[0]
    if not raw:
        return ""
    return raw.decode("utf-8", "replace")


def build_trailer(payload: bytes,
                  key_path: Path,
                  board: int,
                  file_type: int,
                  key_id: bytes,
                  signed_filename: str) -> bytes:
    filename_bytes = signed_filename.encode("utf-8")
    if len(filename_bytes) >= FILENAME_LEN:
        raise SystemExit(f"signed filename too long; max {FILENAME_LEN - 1} UTF-8 bytes")

    trailer = bytearray(TRAILER_LEN)
    trailer[0:32] = MAGIC
    struct.pack_into("<HHII", trailer, 32, 1, TRAILER_LEN, 0, len(payload))
    trailer[44] = file_type & 0xFF
    trailer[45] = board & 0xFF
    trailer[46] = 1  # SHA-256
    trailer[47] = 1  # ECDSA-P256-SHA256
    trailer[48:64] = key_id
    trailer[64:96] = hashlib.sha256(payload).digest()
    trailer[FILENAME_OFF:FILENAME_OFF + len(filename_bytes)] = filename_bytes

    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(trailer[:SIG_OFF])
        meta_path = tf.name
    try:
        der_sig = run_openssl(["dgst", "-sha256", "-sign", str(key_path), meta_path])
    finally:
        os.unlink(meta_path)
    trailer[SIG_OFF:SIG_OFF + 64] = parse_ecdsa_der_sig(der_sig)
    return bytes(trailer)


def put_file(url: str, data: bytes, user: str | None, password: str | None) -> None:
    req = urllib.request.Request(url, data=data, method="PUT")
    req.add_header("Content-Length", str(len(data)))
    req.add_header("Content-Type", "application/octet-stream")
    if user is not None or password is not None:
        token = base64.b64encode(f"{user or ''}:{password or ''}".encode()).decode()
        req.add_header("Authorization", f"Basic {token}")
    with urllib.request.urlopen(req, timeout=120) as r:
        body = r.read().decode("utf-8", "replace")
        if body:
            sys.stderr.write(body)


def device_put_url(device: str, name: str, board: int, store_only: bool) -> str:
    base = device.rstrip("/")
    qname = urllib.parse.quote(name)
    if store_only:
        return f"{base}/files/{qname}"
    board_q = f"&board={board}" if board in (3, 6) else ""
    return f"{base}/jtag?name={qname}{board_q}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path, nargs="?", help="input .bit/.cor/.m65j file")
    ap.add_argument("--key", type=Path, help="explicit P-256 EC private key PEM")
    ap.add_argument("--key-name", default=DEFAULT_KEY_NAME, help="named key under ~/.m65jtag/keys, default 'default'")
    ap.add_argument("--yes", action="store_true", help="create the default private key without prompting")
    ap.add_argument("--keys", action="store_true", help="list local public keys and REMOTE_ENABLE.cfg lines")
    ap.add_argument("--board", choices=("0", "3", "6"), default="0", help="board ID to bind into the signature")
    ap.add_argument("--type", choices=("auto", "any", "bit", "cor", "m65j"), default="auto")
    ap.add_argument("--name", help="destination filename to sign and use for device uploads")
    ap.add_argument("--blank-filename", action="store_true", help="leave filename blank so firmware does not check it")
    ap.add_argument("--bless", action="store_true", help="write a signed local file instead of pushing by default")
    ap.add_argument("-o", "--output", type=Path, help="signed local output path")
    ap.add_argument("--device", help="base board URL, e.g. http://mega65-jtag.local")
    ap.add_argument("--put", help="exact HTTP PUT URL; overrides --device URL construction")
    ap.add_argument("--store-only", action="store_true", help="with --device, PUT to /files/<name> instead of /jtag")
    ap.add_argument("--user", help="HTTP Basic auth user")
    ap.add_argument("--password", help="HTTP Basic auth password")
    ap.add_argument("--print-trusted-key", action="store_true", help="print REMOTE_ENABLE.cfg trusted_key line")
    args = ap.parse_args(argv)

    if args.keys:
        return print_keys()
    if args.input is None:
        ap.error("input is required unless --keys is used")

    payload = args.input.read_bytes()

    name = args.name or args.input.name
    signed_filename = "" if args.blank_filename else name
    type_map = {"any": 0, "bit": 1, "cor": 2, "m65j": 3}
    ftype = file_type_for(args.input) if args.type == "auto" else type_map[args.type]
    existing_name = blessed_filename(payload)

    if args.print_trusted_key or existing_name is None:
        key_path, key_name = resolve_key_path(args.key, args.key_name, args.yes)
        ensure_key(key_path, key_name, args.yes)
        pub = raw_public_key_from_private_key(key_path)
        key_id = hashlib.sha256(pub).digest()[:16]
        if args.print_trusted_key:
            print(f"trusted_key=p256:{pub.hex()}")

    if existing_name is not None:
        print("INFO: file is already blessed; not adding another signature trailer", file=sys.stderr)
        if existing_name and existing_name != name:
            print(f"INFO: existing trailer filename is {existing_name!r}; destination name is {name!r}", file=sys.stderr)
        signed = payload
    else:
        trailer = build_trailer(payload, key_path, int(args.board), ftype, key_id, signed_filename)
        signed = payload + trailer

    put_url = args.put
    if not put_url and args.device:
        put_url = device_put_url(args.device, name, int(args.board), args.store_only)

    wrote = False
    should_write = args.bless or args.output or not put_url
    if should_write:
        out = args.output or default_signed_output(args.input)
        out.write_bytes(signed)
        wrote = True
        print(f"Wrote signed transfer: {out}", file=sys.stderr)

    if put_url and not args.bless:
        print(f"PUT {put_url}", file=sys.stderr)
        put_file(put_url, signed, args.user, args.password)
    elif args.bless and args.device and not wrote:
        raise SystemExit("--bless requested but no output was written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
