#!/usr/bin/env python3
"""Self-contained MEGA65 JTAG client for signing, serial, HTTP, and mirrors."""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import html
import http.cookiejar
import io
import ipaddress
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import asdict, dataclass
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
CONFIG_FILES = (Path(".m65j.config"), Path.home() / ".m65j.config")
SIGNED_SCAN_EXTS = (".bit", ".cor", ".core", ".m65j", ".sha256", ".uf2", ".m65fw", ".bin", ".m65jtheme", ".tar")
M65J_USB_MANUFACTURER = "MEGA65"
M65J_USB_PRODUCT = "MEGA65 Expansion Board Integrated JTAG"
M65J_ATI_PREFIX = "MEGA65 Expansion Board Integrated JTAG"
RASPBERRY_PI_USB_VID = 0x2E8A
PICO_SDK_CDC_PIDS = {0x0009, 0x000A}
P256_SPKI_PREFIX = bytes.fromhex(
    "3059301306072a8648ce3d020106082a8648ce3d030107034200"
)

# Alternate-core catalogue and mirror artifact names.
ALTCORE_PAGES = [
    "https://kugelblitz360.github.io/m65-altcores/computer-cores.html",
    "https://kugelblitz360.github.io/m65-altcores/arcade-cores.html",
    "https://kugelblitz360.github.io/m65-altcores/game-console-cores.html",
]
FILEHOST_ROOTS = {"files.mega65.org", "www.files.mega65.org"}
FILEHOST_ID_RE = re.compile(
    r"https?://files\.mega65\.org/(?:html/main\.php)?\??[^\"' <>\n]*\bid=([0-9a-fA-F-]{36})",
    re.I,
)
HREF_RE = re.compile(r"""href=["']([^"']+)["']""", re.I)
HEADING_RE = re.compile(r"<h([23])[^>]*>(.*?)</h\1>", re.I | re.S)
TAG_RE = re.compile(r"<[^>]+>")
ALLOWED_DOWNLOAD_EXTS = (".zip", ".bit", ".cor", ".core", ".m65j")
CORE_FILE_EXTS = (".bit", ".cor", ".core", ".m65j")
COR_MAGIC = b"MEGA65BITSTREAM0"
PUBLIC_FIRMWARE_FILENAME = "mega65-pico-jtag.uf2"
OTA_FIRMWARE_FILENAME = "mega65-integrated-jtag-firmware.uf2"
SDCARD_BOOTSTRAP_FILENAME = "sdcard-files.zip"
SDCARD_BOOTSTRAP_ALL_CORES_FILENAME = "sdcard-files-all-cores.zip"


CLI_SERIAL_DEVICE = None
CLI_WEB_URL = None


def load_serial_module():
    try:
        import serial
    except ImportError:
        print("Install pyserial for serial-port commands: python3 -m pip install pyserial", file=sys.stderr)
        raise
    return serial


def load_list_ports_module():
    try:
        from serial.tools import list_ports
    except ImportError:
        print("Install pyserial for serial-port auto-detection: python3 -m pip install pyserial", file=sys.stderr)
        raise
    return list_ports


def port_info_text(port):
    values = (
        getattr(port, "device", None),
        getattr(port, "name", None),
        getattr(port, "description", None),
        getattr(port, "manufacturer", None),
        getattr(port, "product", None),
        getattr(port, "serial_number", None),
        getattr(port, "hwid", None),
    )
    return " ".join(str(v) for v in values if v)


def format_port_info(port):
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    vidpid = f"{vid:04x}:{pid:04x}" if vid is not None and pid is not None else "vid:pid=?"
    parts = [getattr(port, "device", "?"), vidpid]
    for attr in ("manufacturer", "product", "serial_number"):
        value = getattr(port, attr, None)
        if value:
            parts.append(str(value))
    return " ".join(parts)


IDENTITY_RE = re.compile(r"\br([036])\s*:\s*([A-Za-z0-9._-]+)\b", re.IGNORECASE)


def parse_identity_text(text):
    match = IDENTITY_RE.search(text or "")
    if not match:
        return None
    board = f"r{match.group(1)}".lower()
    name = match.group(2)
    return {"identity": f"{board}:{name}", "board": board, "name": name}


def identity_matches(identity, target):
    if not identity or not target:
        return False
    target_l = target.lower()
    if ":" in target_l:
        return identity.get("identity", "").lower() == target_l
    return identity.get("name", "").lower() == target_l


def is_ip_literal(value):
    try:
        ipaddress.ip_address(str(value).strip("[]"))
        return True
    except ValueError:
        return False


def is_web_target_arg(value):
    lower = str(value).lower()
    return (
        "://" in lower
        or lower == "localhost"
        or lower.startswith("[")
        or is_ip_literal(value)
        or ("." in lower and "/" not in lower and "\\" not in lower)
    )


def looks_like_machine_name(value):
    text = str(value)
    if not text or len(text) > 24:
        return False
    if "/" in text or "\\" in text or ":" in text:
        return False
    if is_web_target_arg(text) or is_serial_port_arg(text):
        return False
    return re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", text) is not None


def looks_like_machine_target(value):
    text = str(value)
    return looks_like_machine_name(text) or re.fullmatch(r"r[036]:[A-Za-z0-9][A-Za-z0-9._-]{0,23}", text, re.IGNORECASE) is not None


def is_m65j_descriptor_port(port):
    text = port_info_text(port).lower()
    product = str(getattr(port, "product", "") or "").lower()
    manufacturer = str(getattr(port, "manufacturer", "") or "").lower()
    return (
        M65J_USB_PRODUCT.lower() in text
        or ("mega65" in text and "jtag" in text)
        or (manufacturer == M65J_USB_MANUFACTURER.lower() and "jtag" in product)
    )


def is_pico_cdc_port(port):
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    if vid == RASPBERRY_PI_USB_VID and (pid in PICO_SDK_CDC_PIDS or pid is None):
        return True
    text = port_info_text(port).lower()
    return "raspberry pi" in text and ("pico" in text or "board cdc" in text)


def _serial_read_until_ok(ser, deadline):
    lines = []
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").strip()
        if text:
            lines.append(text)
        if text == "OK" or text.startswith("ERR ") or text.startswith("ERROR:") or text == "NO CARRIER":
            break
    return lines


def probe_m65j_port_identity(serial, device, baud):
    try:
        with serial.Serial(device, baud, timeout=0.08, write_timeout=0.25) as ser:
            time.sleep(0.05)
            ser.reset_input_buffer()
            ser.write(b"ATI\n")
            ser.flush()
            seen = _serial_read_until_ok(ser, time.monotonic() + 0.9)
            joined = "\n".join(seen)
            if M65J_ATI_PREFIX not in joined and "pico-m65jtag" not in joined:
                return None
            identity = parse_identity_text(joined)
            ser.reset_input_buffer()
            ser.write(b"AT+MACHINE?\n")
            ser.flush()
            machine_lines = _serial_read_until_ok(ser, time.monotonic() + 0.8)
            identity = parse_identity_text("\n".join(machine_lines)) or identity
            return {
                "kind": "serial",
                "device": device,
                "port": device,
                "identity": identity["identity"] if identity else "",
                "board": identity["board"] if identity else "r0",
                "name": identity["name"] if identity else "",
                "detail": joined,
            }
    except (OSError, getattr(serial, "SerialException", OSError)):
        return None


def probe_m65j_port(serial, device, baud):
    return probe_m65j_port_identity(serial, device, baud) is not None


def usb_probe_candidates():
    list_ports = load_list_ports_module()
    ports = sorted(list_ports.comports(), key=lambda p: getattr(p, "device", ""))
    descriptor_matches = [p for p in ports if is_m65j_descriptor_port(p)]
    pico_matches = [p for p in ports if is_pico_cdc_port(p) and p not in descriptor_matches]
    return descriptor_matches, pico_matches


def discover_usb_machines(baud):
    serial = load_serial_module()
    descriptor_matches, pico_matches = usb_probe_candidates()
    machines = []
    for port in [*descriptor_matches, *pico_matches]:
        info = probe_m65j_port_identity(serial, port.device, baud)
        if info:
            info["description"] = format_port_info(port)
            machines.append(info)
    return machines


def autodetect_serial_port(baud):
    serial = load_serial_module()
    descriptor_matches, pico_matches = usb_probe_candidates()
    if len(descriptor_matches) == 1:
        port = descriptor_matches[0]
        print(f"INFO: auto-detected MEGA65 JTAG serial device: {format_port_info(port)}", file=sys.stderr)
        return port.device
    if len(descriptor_matches) > 1:
        details = "\n  ".join(format_port_info(p) for p in descriptor_matches)
        raise SystemExit(f"Multiple MEGA65 JTAG serial devices found; pass -s/--device:\n  {details}")

    candidates = pico_matches
    if not candidates:
        return None

    probed = [p for p in candidates if probe_m65j_port(serial, p.device, baud)]
    if len(probed) == 1:
        port = probed[0]
        print(f"INFO: auto-detected MEGA65 JTAG serial device by ATI probe: {format_port_info(port)}", file=sys.stderr)
        return port.device
    if len(probed) > 1:
        details = "\n  ".join(format_port_info(p) for p in probed)
        raise SystemExit(f"Multiple MEGA65 JTAG serial devices answered ATI; pass -s/--device:\n  {details}")
    return None


def run_openssl(args, input_data=None):
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


def key_path_for_name(name):
    safe = "".join(c for c in name if c.isalnum() or c in ("-", "_", ".")).strip(".")
    if not safe:
        safe = DEFAULT_KEY_NAME
    return KEY_DIR / f"{safe}.pem"


def resolve_key_path(key, key_name, assume_yes):
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


def ensure_key(path, name, assume_yes):
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


def der_read_len(data, pos):
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


def der_expect(data, pos, tag):
    if pos >= len(data) or data[pos] != tag:
        raise ValueError(f"expected DER tag 0x{tag:02x}")
    length, pos = der_read_len(data, pos + 1)
    end = pos + length
    if end > len(data):
        raise ValueError("short DER value")
    return data[pos:end], end


def parse_ecdsa_der_sig(sig):
    seq, end = der_expect(sig, 0, 0x30)
    if end != len(sig):
        raise ValueError("trailing DER signature bytes")
    r, pos = der_expect(seq, 0, 0x02)
    s, pos = der_expect(seq, pos, 0x02)
    if pos != len(seq):
        raise ValueError("trailing ECDSA sequence bytes")

    def fixed32(v):
        while len(v) > 0 and v[0] == 0:
            v = v[1:]
        if len(v) > 32:
            raise ValueError("ECDSA integer too large")
        return b"\x00" * (32 - len(v)) + v

    return fixed32(r) + fixed32(s)


def raw_public_key_from_private_key(key_path):
    der = run_openssl(["pkey", "-in", str(key_path), "-pubout", "-outform", "DER"])
    for pos in range(len(der) - 65, -1, -1):
        if der[pos] == 0x04 and pos + 65 <= len(der):
            return der[pos:pos + 65]
    raise ValueError("could not extract raw P-256 public key from openssl DER")


def trusted_key_line(key_path):
    pub = raw_public_key_from_private_key(key_path)
    return f"trusted_key=p256:{pub.hex()}"


def iter_known_keys():
    keys = []
    if KEY_DIR.exists():
        for path in sorted(KEY_DIR.glob("*.pem")):
            keys.append((path.stem, path))
    if DEFAULT_KEY.exists() and all(path != DEFAULT_KEY for _, path in keys):
        keys.append(("legacy-default", DEFAULT_KEY))
    return keys


def print_keys():
    keys = iter_known_keys()
    if not keys:
        print(f"No signing keys found in {KEY_DIR}")
        print("Create one with: tools/m65j.py bless --yes core.bit")
        return 0
    print("Configured local signing keys:\n")
    for name, path in keys:
        print(f"[{name}] {path}")
        try:
            print(f"  {trusted_key_line(path)}")
        except Exception as e:
            print(f"  ERROR: {e}")
    print("\nTo trust a key, copy its trusted_key= line into mega65-jtag.cfg on the SD card.")
    print("For enforcement, also set: require_signatures=1")
    return 0


def file_type_for(path):
    ext = path.suffix.lower()
    if ext == ".bit":
        return 1
    if ext in {".cor", ".core"}:
        return 2
    if ext == ".m65j":
        return 3
    if ext in {".uf2", ".m65fw", ".bin"}:
        return 4
    if ext in {".m65jtheme", ".tar"}:
        return 5
    return 0


def default_signed_output(path):
    if path.suffix:
        return path.with_name(f"{path.stem}.signed{path.suffix}")
    return path.with_name(f"{path.name}.signed")


def blessed_filename(data):
    if len(data) < TRAILER_LEN or data[-TRAILER_LEN:-TRAILER_LEN + len(MAGIC)] != MAGIC:
        return None
    raw = data[-TRAILER_LEN + FILENAME_OFF:-TRAILER_LEN + FILENAME_OFF + FILENAME_LEN]
    raw = raw.split(b"\x00", 1)[0]
    if not raw:
        return ""
    return raw.decode("utf-8", "replace")


def der_len(n):
    if n < 0x80:
        return bytes([n])
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def der_int(raw):
    value = bytes(raw).lstrip(b"\x00") or b"\x00"
    if value[0] & 0x80:
        value = b"\x00" + value
    return b"\x02" + der_len(len(value)) + value


def ecdsa_der_from_raw(raw_sig):
    if len(raw_sig) != 64:
        raise ValueError("raw ECDSA signature must be 64 bytes")
    body = der_int(raw_sig[:32]) + der_int(raw_sig[32:])
    return b"\x30" + der_len(len(body)) + body


def parse_trusted_key_value(value):
    value = value.strip()
    lower = value.lower()
    if lower.startswith("p256:"):
        value = value[5:]
    elif lower.startswith("ecdsa-p256:"):
        value = value[11:]
    compact = "".join(c for c in value if c not in ":- \t\r\n")
    try:
        raw = bytes.fromhex(compact)
    except ValueError:
        return None
    if len(raw) != 65 or raw[0] != 0x04:
        return None
    return raw


def trusted_keys_from_remote_config(path):
    keys = []
    text = Path(path).read_text(encoding="utf-8")
    for line_no, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip().lower() not in {"trusted_key", "public_key", "signature_key"}:
            continue
        pub = parse_trusted_key_value(value)
        if pub is None:
            print(f"WARNING {path}:{line_no}: ignored malformed trusted key", file=sys.stderr)
            continue
        keys.append((f"{Path(path).name}:{line_no}", pub))
    return keys


def local_trusted_keys():
    keys = []
    for name, path in iter_known_keys():
        try:
            keys.append((name, raw_public_key_from_private_key(path)))
        except Exception as exc:  # noqa: BLE001
            print(f"WARNING skipped local key {path}: {exc}", file=sys.stderr)
    return keys


def verify_p256_raw_signature(pubkey, metadata, raw_sig):
    pub_der = P256_SPKI_PREFIX + pubkey
    sig_der = ecdsa_der_from_raw(raw_sig)
    temps = []
    try:
        for data in (pub_der, sig_der, metadata):
            tf = tempfile.NamedTemporaryFile(delete=False)
            tf.write(data)
            tf.close()
            temps.append(tf.name)
        p = subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-keyform",
                "DER",
                "-verify",
                temps[0],
                "-signature",
                temps[1],
                temps[2],
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        return p.returncode == 0
    except FileNotFoundError:
        raise SystemExit("openssl not found in PATH")
    finally:
        for name in temps:
            try:
                os.unlink(name)
            except OSError:
                pass


def trailer_filename(trailer):
    raw = trailer[FILENAME_OFF:FILENAME_OFF + FILENAME_LEN]
    if raw and raw[0] and b"\x00" not in raw:
        raise ValueError("signature filename is not terminated")
    raw = raw.split(b"\x00", 1)[0]
    return raw.decode("utf-8", "replace") if raw else ""


def check_signed_bytes(data, path, trusted_keys, check_filename=True, check_type=True):
    if len(data) < TRAILER_LEN or data[-TRAILER_LEN:-TRAILER_LEN + len(MAGIC)] != MAGIC:
        return "uncursed", "no signature trailer"

    payload = data[:-TRAILER_LEN]
    trailer = data[-TRAILER_LEN:]
    version, header_len = struct.unpack_from("<HH", trailer, 32)
    payload_len = struct.unpack_from("<I", trailer, 40)[0]
    file_type = trailer[44]
    board = trailer[45]
    hash_alg = trailer[46]
    sig_alg = trailer[47]
    key_id = trailer[48:64]
    payload_hash = trailer[64:96]
    raw_sig = trailer[SIG_OFF:SIG_OFF + 64]

    if version != 1 or header_len != TRAILER_LEN:
        return "cursed", f"unsupported signature block version/header ({version}/{header_len})"
    if payload_len != len(payload):
        return "cursed", f"signature payload length mismatch ({payload_len} != {len(payload)})"
    if hash_alg != 1 or sig_alg != 1:
        return "cursed", f"unsupported signature algorithms ({hash_alg}/{sig_alg})"
    if hashlib.sha256(payload).digest() != payload_hash:
        return "cursed", "payload SHA-256 mismatch"
    if check_type:
        expected_type = file_type_for(path)
        if file_type != 0 and expected_type != 0 and file_type != expected_type:
            return "cursed", f"file type mismatch (signed={file_type}, path={expected_type})"
    try:
        signed_name = trailer_filename(trailer)
    except ValueError as exc:
        return "cursed", str(exc)
    if check_filename and signed_name and signed_name != path.name:
        return "cursed", f"filename mismatch (signed={signed_name!r}, path={path.name!r})"

    if not trusted_keys:
        return "cursed", "no trusted keys available to verify signature"

    metadata = trailer[:SIG_OFF]
    matched_key_id = 0
    for name, pubkey in trusted_keys:
        if any(key_id):
            have_key_id = hashlib.sha256(pubkey).digest()[:16]
            if have_key_id != key_id:
                continue
            matched_key_id += 1
        if verify_p256_raw_signature(pubkey, metadata, raw_sig):
            details = f"key={name} board={board} type={file_type}"
            if signed_name:
                details += f" name={signed_name}"
            return "blessed", details

    if any(key_id) and matched_key_id == 0:
        return "cursed", f"no trusted key matches key_id={key_id.hex()}"
    return "cursed", "signature verification failed"


def collect_check_paths(inputs, all_files=False):
    paths = []
    for value in inputs:
        path = Path(value)
        if path.is_dir():
            for child in sorted(path.rglob("*")):
                if child.is_file() and (all_files or child.suffix.lower() in SIGNED_SCAN_EXTS):
                    paths.append(child)
        else:
            paths.append(path)
    return paths


def check_main(argv):
    ap = argparse.ArgumentParser(
        prog="m65j.py check",
        description="Report signed-file blessedness for local files.",
    )
    ap.add_argument("paths", nargs="+", help="files or directories to inspect")
    ap.add_argument("--trusted-key", action="append", default=[], help="raw trusted key, e.g. p256:04...")
    ap.add_argument("--remote-config", action="append", type=Path, default=[],
                    help="mega65-jtag.cfg to read trusted_key lines from")
    ap.add_argument("--no-local-keys", action="store_true", help="do not use local ~/.m65jtag signing keys")
    ap.add_argument("--no-filename-check", action="store_true", help="do not mark renamed signed files as cursed")
    ap.add_argument("--no-type-check", action="store_true", help="do not compare signed type with filename extension")
    ap.add_argument("--all", action="store_true", help="when checking directories, include every file")
    args = ap.parse_args(argv)

    trusted = []
    if not args.no_local_keys:
        trusted.extend(local_trusted_keys())
    for value in args.trusted_key:
        pub = parse_trusted_key_value(value)
        if pub is None:
            raise SystemExit(f"bad --trusted-key value: {value}")
        trusted.append(("cli", pub))
    for path in args.remote_config:
        trusted.extend(trusted_keys_from_remote_config(path))

    paths = collect_check_paths(args.paths, args.all)
    if not paths:
        raise SystemExit("no files matched")

    rc = 0
    for path in paths:
        try:
            data = path.read_bytes()
        except OSError as exc:
            print(f"[cursed]   {path} - cannot read: {exc}")
            rc = 1
            continue
        status, detail = check_signed_bytes(
            data,
            path,
            trusted,
            check_filename=not args.no_filename_check,
            check_type=not args.no_type_check,
        )
        pad = " " * max(1, 9 - len(status))
        suffix = f" - {detail}" if status == "cursed" else (f" - {detail}" if status == "blessed" else "")
        print(f"[{status}]{pad}{path}{suffix}")
        if status == "cursed":
            rc = 1
    return rc


def build_trailer(payload, key_path, board, file_type, key_id, signed_filename):
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


def put_file(url, data, user, password):
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


def normalize_device(device):
    if not device:
        return None
    device = device.strip().rstrip("/")
    if not device:
        return None
    if "://" not in device:
        device = f"http://{device}"
    return device


def local_ipv4_addresses():
    addrs = set()
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM)
        for info in infos:
            ip = info[4][0]
            addr = ipaddress.ip_address(ip)
            if not addr.is_loopback and not addr.is_link_local:
                addrs.add(ip)
    except OSError:
        pass

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            addr = ipaddress.ip_address(ip)
            if not addr.is_loopback and not addr.is_link_local:
                addrs.add(ip)
    except OSError:
        pass

    try:
        p = subprocess.run(
            ["ip", "-o", "-4", "addr", "show", "scope", "global"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        for line in p.stdout.splitlines():
            match = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)/", line)
            if match:
                addrs.add(match.group(1))
    except (OSError, ValueError):
        pass

    return sorted(addrs)


def local_24_scan_hosts():
    hosts = []
    seen = set()
    for ip in local_ipv4_addresses():
        try:
            network = ipaddress.ip_network(f"{ip}/24", strict=False)
        except ValueError:
            continue
        for host in network.hosts():
            text = str(host)
            if text not in seen:
                seen.add(text)
                hosts.append(text)
    return hosts


def probe_http_identity(host, timeout=0.25):
    try:
        with socket.create_connection((host, 80), timeout=timeout) as s:
            s.settimeout(timeout)
            req = (
                f"GET /identity HTTP/1.0\r\n"
                f"Host: {host}\r\n"
                "Connection: close\r\n"
                "Accept: text/plain\r\n"
                "\r\n"
            ).encode("ascii")
            s.sendall(req)
            chunks = []
            total = 0
            while total < 2048:
                chunk = s.recv(512)
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
    except OSError:
        return None

    raw = b"".join(chunks).decode("utf-8", "replace")
    if not raw.startswith("HTTP/"):
        return None
    status = raw.splitlines()[0] if raw else ""
    if " 200 " not in status:
        return None
    body = raw.split("\r\n\r\n", 1)[-1].strip()
    identity = parse_identity_text(body)
    if not identity:
        return None
    return {
        "kind": "web",
        "device": f"http://{host}",
        "url": f"http://{host}",
        "identity": identity["identity"],
        "board": identity["board"],
        "name": identity["name"],
        "description": f"http://{host}",
    }


def discover_web_machines(match_name=None):
    hosts = local_24_scan_hosts()
    if not hosts:
        return []
    machines = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as executor:
        futures = {executor.submit(probe_http_identity, host): host for host in hosts}
        for future in concurrent.futures.as_completed(futures):
            info = future.result()
            if not info:
                continue
            if match_name and not identity_matches(info, match_name):
                continue
            machines.append(info)
    return sorted(machines, key=lambda m: (m.get("name", ""), m.get("device", "")))


def resolve_machine_name(target, baud, quiet=False):
    usb_matches = [m for m in discover_usb_machines(baud) if identity_matches(m, target)]
    if len(usb_matches) == 1:
        info = usb_matches[0]
        if not quiet:
            print(f"INFO: matched {target} on USB: {info.get('description', info['device'])}", file=sys.stderr)
        return "serial", info["device"]
    if len(usb_matches) > 1:
        details = "\n  ".join(f"{m.get('identity') or 'r0:unnamed'} {m.get('description', m['device'])}" for m in usb_matches)
        raise SystemExit(f"Multiple USB machines match {target}; use -s/--device:\n  {details}")

    if not quiet:
        print(f"INFO: no USB machine named {target}; scanning local /24 HTTP port 80", file=sys.stderr)
    web_matches = discover_web_machines(target)
    if len(web_matches) == 1:
        info = web_matches[0]
        if not quiet:
            print(f"INFO: matched {target} on web: {info['url']}", file=sys.stderr)
        return "web", info["url"]
    if len(web_matches) > 1:
        details = "\n  ".join(f"{m.get('identity')} {m.get('url')}" for m in web_matches)
        raise SystemExit(f"Multiple web machines match {target}; pass a URL:\n  {details}")
    raise SystemExit(f"No MEGA65 JTAG machine named {target} found on USB or local /24 HTTP")


def resolve_target_arg(target, baud, quiet=False):
    if is_serial_port_arg(target):
        return "serial", target
    if is_web_target_arg(target):
        return "web", normalize_device(target)
    if looks_like_machine_target(target):
        return resolve_machine_name(target, baud, quiet=quiet)
    return None, target


def list_available_machines(baud):
    rows = []
    for info in discover_usb_machines(baud):
        rows.append(("usb", info.get("identity") or "r0:unnamed", info.get("description", info["device"])))
    for info in discover_web_machines():
        rows.append(("web", info.get("identity") or "r0:unnamed", info.get("url", info["device"])))
    if not rows:
        print("No MEGA65 JTAG machines found on USB or local /24 HTTP.")
        return 1
    for kind, identity, where in sorted(rows, key=lambda r: (r[1].lower(), r[0], r[2])):
        print(f"{kind:6} {identity:32} {where}")
    return 0


def read_client_config():
    cfg = {}
    for path in CONFIG_FILES:
        try:
            text = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            continue
        for raw in text.splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            cfg[key.strip().lower()] = value.strip()
    return cfg


def configured_serial_port(cfg=None):
    if CLI_SERIAL_DEVICE:
        return CLI_SERIAL_DEVICE
    cfg = read_client_config() if cfg is None else cfg
    for key in ("serial", "serial_port", "tty", "usb", "usb_device"):
        value = cfg.get(key)
        if value:
            return value
    value = cfg.get("device")
    if value and is_serial_port_arg(value):
        return value
    return None


def configured_machine(cfg=None):
    cfg = read_client_config() if cfg is None else cfg
    for key in ("machine", "machine_name", "name", "target"):
        value = cfg.get(key)
        if value and looks_like_machine_target(value):
            return value
    value = cfg.get("device")
    if value and looks_like_machine_target(value):
        return value
    return None


def configured_device(cfg=None):
    if CLI_WEB_URL:
        return normalize_device(CLI_WEB_URL)
    cfg = read_client_config() if cfg is None else cfg
    device = cfg.get("url") or cfg.get("web_url") or cfg.get("http_url") or cfg.get("base_url")
    legacy_device = cfg.get("device")
    if not device and legacy_device and not is_serial_port_arg(legacy_device) and not looks_like_machine_target(legacy_device):
        device = legacy_device
    if not device:
        ip = cfg.get("ip") or cfg.get("host")
        if ip:
            port = cfg.get("port")
            device = f"{ip}:{port}" if port else ip
    return normalize_device(device)


def require_device(explicit=None):
    device = normalize_device(explicit) if explicit else configured_device()
    if device:
        return device
    raise SystemExit(
        "No web URL configured. Add url=http://<pico-ip> or ip=<pico-ip> "
        "to .m65j.config or ~/.m65j.config, or pass -u/--url. "
        "For USB serial, pass -s/--device or connect exactly one MEGA65 JTAG Pico CDC device."
    )


def config_auth(user=None, password=None):
    cfg = read_client_config()
    return (
        user if user is not None else cfg.get("user") or cfg.get("username"),
        password if password is not None else cfg.get("password"),
    )


def http_request(url, user=None, password=None, data=None, method=None):
    req = urllib.request.Request(url, data=data, method=method or ("PUT" if data is not None else "GET"))
    if data is not None:
        req.add_header("Content-Length", str(len(data)))
        req.add_header("Content-Type", "application/octet-stream")
    if user is not None or password is not None:
        token = base64.b64encode(f"{user or ''}:{password or ''}".encode()).decode()
        req.add_header("Authorization", f"Basic {token}")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.read(), r.headers.get_content_type()
    except urllib.error.HTTPError as e:
        body = e.read(240).decode("utf-8", "replace").strip()
        detail = f": {body}" if body else ""
        raise SystemExit(f"ERR HTTP {e.code} {e.reason} from {url}{detail}")
    except (ConnectionResetError, TimeoutError, socket.timeout, urllib.error.URLError, OSError) as e:
        raise SystemExit(f"ERR HTTP request failed for {url}: {e}")


def device_put_url(device, name, board, store_only):
    base = require_device(device)
    qname = urllib.parse.quote(name)
    if store_only:
        return f"{base}/files/{qname}"
    board_q = f"&board={board}" if board in (3, 6) else ""
    return f"{base}/jtag?name={qname}{board_q}"


def web_url(device, path):
    return f"{require_device(device)}{path}"


def write_response_body(body, output):
    if output:
        Path(output).write_bytes(body)
    else:
        sys.stdout.buffer.write(body)


def web_status(device=None, board=None, user=None, password=None):
    query = f"?board={board}" if board in ("3", "6", 3, 6) else ""
    body, ctype = http_request(web_url(device, f"/index.html{query}"), user, password)
    if ctype.startswith("text/") or ctype in {"application/json", "application/xhtml+xml"}:
        print(body.decode("utf-8", "replace"), end="")
    else:
        sys.stdout.buffer.write(body)
    return 0


def web_file_get(device, remote_path, output, user=None, password=None, downloads=False):
    if not remote_path:
        raise SystemExit("missing remote filename")
    endpoint = "/downloads/" if downloads else "/files/"
    encoded = urllib.parse.quote(remote_path.lstrip("/"))
    body, _ = http_request(web_url(device, endpoint + encoded), user, password)
    if output is None:
        output = Path(remote_path).name
    write_response_body(body, output)
    if output:
        print(f"Wrote {output}", file=sys.stderr)
    return 0


def web_jtag_file(device, remote_path, board=None, user=None, password=None):
    if not remote_path:
        raise SystemExit("missing SD core filename")
    q = urllib.parse.urlencode({"file": remote_path})
    if board in ("3", "6", 3, 6):
        q += f"&board={board}"
    body, _ = http_request(web_url(device, f"/jtag?{q}"), user, password)
    print(body.decode("utf-8", "replace"), end="")
    return 0


def signing_main(argv):
    ap = argparse.ArgumentParser(
        description="Bless, upload, or JTAG-push a MEGA65 JTAG core file.",
    )
    ap.add_argument("input", type=Path, nargs="?", help="input .bit/.cor/.m65j file")
    ap.add_argument("--key", type=Path, help="explicit P-256 EC private key PEM")
    ap.add_argument("--key-name", default=DEFAULT_KEY_NAME, help="named key under ~/.m65jtag/keys, default 'default'")
    ap.add_argument("--yes", action="store_true", help="create the default private key without prompting")
    ap.add_argument("--keys", action="store_true", help="list local public keys and mega65-jtag.cfg lines")
    ap.add_argument("--board", choices=("0", "3", "6"), default="0", help="board ID to bind into the signature")
    ap.add_argument("--type", choices=("auto", "any", "bit", "cor", "m65j", "firmware", "theme"), default="auto")
    ap.add_argument("--name", help="destination filename to sign and use for device uploads")
    ap.add_argument("--blank-filename", action="store_true", help="leave filename blank so firmware does not check it")
    ap.add_argument("--bless", action="store_true", help="write a signed local file instead of pushing by default")
    ap.add_argument("-o", "--output", type=Path, help="signed local output path")
    ap.add_argument("--device", "--url", "-u", dest="device", help="base board URL, e.g. http://mega65-jtag.local")
    ap.add_argument("--put", help="exact HTTP PUT URL; overrides --device URL construction")
    ap.add_argument("--store-only", action="store_true", help="with --device, PUT to /files/<name> instead of /jtag")
    ap.add_argument("--user", help="HTTP Basic auth user")
    ap.add_argument("--password", help="HTTP Basic auth password")
    ap.add_argument("--print-trusted-key", action="store_true", help="print mega65-jtag.cfg trusted_key line")
    args = ap.parse_args(argv)

    if args.keys:
        return print_keys()
    if args.input is None:
        ap.error("input is required unless --keys is used")

    payload = args.input.read_bytes()

    name = args.name or args.input.name
    signed_filename = "" if args.blank_filename else name
    type_map = {"any": 0, "bit": 1, "cor": 2, "m65j": 3, "firmware": 4, "theme": 5}
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
        user, password = config_auth(args.user, args.password)
        put_file(put_url, signed, user, password)
    elif args.bless and args.device and not wrote:
        raise SystemExit("--bless requested but no output was written")
    return 0


# Alternate-core catalogue and downloads.


def sdcard_bootstrap_board_cores_filename(board: str) -> str:
    return f"sdcard-files-{board.lower()}-cores.zip"


@dataclass
class CoreRef:
    title: str
    filehost_id: str
    url: str
    source_page: str
    board_hint: str


@dataclass
class DownloadResult:
    title: str
    filehost_id: str
    archive_url: str | None
    extracted: str | None
    status: str


@dataclass
class DownloadCandidate:
    url: str
    filename: str | None = None
    source: str = "link"


@dataclass
class ManifestArtifact:
    kind: str
    path: Path
    rel: str
    version: str = ""
    build: str = ""
    name: str = ""


def core_file_paths(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted(
        p for p in root.rglob("*")
        if p.is_file() and p.suffix.lower() in CORE_FILE_EXTS
    )


def strip_signature_trailer(data: bytes) -> bytes:
    if len(data) >= TRAILER_LEN and data[-TRAILER_LEN:-TRAILER_LEN + len(MAGIC)] == MAGIC:
        return data[:-TRAILER_LEN]
    return data


def clean_text(s: str) -> str:
    return " ".join(html.unescape(TAG_RE.sub(" ", s)).split())


def request(url: str, cookie: str | None = None, method: str = "GET") -> urllib.request.Request:
    headers = {
        "User-Agent": "mega65-altcore-fetch/1.0",
        "Accept": "*/*",
    }
    if cookie:
        headers["Cookie"] = cookie
    return urllib.request.Request(url, headers=headers, method=method)


def request_with_data(url: str, fields: dict[str, str], cookie: str | None = None) -> urllib.request.Request:
    data = urllib.parse.urlencode(fields).encode("utf-8")
    headers = {
        "User-Agent": "mega65-altcore-fetch/1.0",
        "Accept": "*/*",
        "Content-Type": "application/x-www-form-urlencoded",
        "Content-Length": str(len(data)),
    }
    if cookie:
        headers["Cookie"] = cookie
    return urllib.request.Request(url, data=data, headers=headers, method="POST")


def fetch_bytes(url: str, cookie: str | None = None, timeout: float = 10.0) -> tuple[bytes, str, str]:
    with urllib.request.urlopen(request(url, cookie), timeout=timeout) as resp:
        data = resp.read()
        ctype = resp.headers.get("Content-Type", "")
        final_url = resp.geturl()
    return data, ctype, final_url


def post_json(url: str, fields: dict[str, str], cookie: str | None = None, timeout: float = 10.0):
    with urllib.request.urlopen(request_with_data(url, fields, cookie), timeout=timeout) as resp:
        data = resp.read()
    return json.loads(data.decode("utf-8", errors="replace"))


def fetch_text(url: str, cookie: str | None = None) -> str:
    data, ctype, _ = fetch_bytes(url, cookie)
    charset = "utf-8"
    m = re.search(r"charset=([^;\s]+)", ctype, re.I)
    if m:
        charset = m.group(1)
    return data.decode(charset, errors="replace")


def nearest_heading(headings: list[tuple[int, str]], pos: int) -> str:
    title = "unknown-core"
    for hpos, text in headings:
        if hpos > pos:
            break
        title = text
    return title


def board_hint_from_context(context: str) -> str:
    text = clean_text(context).lower()
    has_r3 = bool(re.search(r"\br3\b|r3-only|r3 only|rev(?:ision)?\s*3", text))
    has_r6 = bool(re.search(r"\br6\b|r6-only|r6 only|rev(?:ision)?\s*6", text))
    if has_r3 and not has_r6:
        return "r3"
    if has_r6 and not has_r3:
        return "r6"
    if has_r3 and has_r6:
        return "both"
    return "unknown"


def board_label(board_id: int) -> str:
    if board_id == 3:
        return "r3"
    if board_id == 6:
        return "r6"
    return "unknown"


def expand_source_pages(pages: list[str] | None) -> list[str]:
    if not pages:
        return ALTCORE_PAGES
    expanded: list[str] = []
    for page in pages:
        parsed = urllib.parse.urlparse(page)
        if parsed.netloc.lower() in FILEHOST_ROOTS and parsed.path in ("", "/") and not parsed.query:
            expanded.extend(ALTCORE_PAGES)
        else:
            expanded.append(page)
    return expanded


def scrape_altcore_refs(cookie: str | None = None, pages: list[str] | None = None) -> list[CoreRef]:
    refs: list[CoreRef] = []
    seen: set[tuple[str, str]] = set()
    for page in expand_source_pages(pages):
        text = fetch_text(page, cookie)
        headings = [(m.start(), clean_text(m.group(2))) for m in HEADING_RE.finditer(text)]
        for m in FILEHOST_ID_RE.finditer(text):
            file_id = m.group(1).lower()
            href = m.group(0)
            title = nearest_heading(headings, m.start())
            context = text[max(0, m.start() - 400) : min(len(text), m.end() + 180)]
            hint = board_hint_from_context(context)
            key = (file_id, hint)
            if key in seen:
                continue
            seen.add(key)
            refs.append(CoreRef(title=title, filehost_id=file_id, url=href, source_page=page, board_hint=hint))
    return refs


def allowed_download_ext(value: str | None) -> str | None:
    if not value:
        return None
    parsed = urllib.parse.urlparse(html.unescape(value))
    path = urllib.parse.unquote(parsed.path or value)
    lower = path.lower()
    for ext in ALLOWED_DOWNLOAD_EXTS:
        if lower.endswith(ext):
            return ext
    return None


def safe_download_filename(name: str | None, fallback_url: str, default: str) -> str:
    candidate = name or Path(urllib.parse.unquote(urllib.parse.urlparse(fallback_url).path)).name or default
    candidate = Path(candidate).name
    candidate = re.sub(r"[^A-Za-z0-9._ -]+", "-", candidate).strip(" .-_")
    if not candidate:
        candidate = default
    ext = allowed_download_ext(candidate)
    if ext == ".core":
        candidate = f"{Path(candidate).stem}.cor"
    return candidate


def filehost_detail_candidates(file_id: str, cookie: str | None, verbose: bool = False) -> tuple[list[DownloadCandidate], str | None]:
    url = "https://files.mega65.org/php/readfiledetail.php"
    try:
        rows = post_json(url, {"fileid": file_id}, cookie)
    except Exception as exc:  # noqa: BLE001
        return [], f"filehost detail failed: {exc}"

    if not isinstance(rows, list) or not rows:
        return [], "filehost detail returned no rows"

    candidates: list[DownloadCandidate] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        state = str(row.get("state", ""))
        if state and state != "OK":
            return [], f"filehost detail state={state}: {row.get('message', '')}"
        location = str(row.get("location") or "")
        filename = str(row.get("filename") or "")
        if not location:
            continue
        if not (allowed_download_ext(filename) or allowed_download_ext(location)):
            if verbose:
                print(f"  detail ignored non-core filename/location: {filename or location}", flush=True)
            continue
        candidates.append(DownloadCandidate(
            urllib.parse.urljoin("https://files.mega65.org/html/filedetail.php", html.unescape(location)),
            filename=filename or None,
            source="readfiledetail",
        ))
    return candidates, None if candidates else "filehost detail returned no .zip/.cor/.core/.bit location"


def candidate_archive_urls(file_id: str, page_html: str, page_url: str) -> list[DownloadCandidate]:
    urls: list[DownloadCandidate] = []
    for href in HREF_RE.findall(page_html):
        absolute = urllib.parse.urljoin(page_url, html.unescape(href))
        if allowed_download_ext(absolute):
            urls.append(DownloadCandidate(absolute, source="html-link"))

    deduped: list[DownloadCandidate] = []
    seen: set[str] = set()
    for candidate in urls:
        if candidate.url not in seen:
            seen.add(candidate.url)
            deduped.append(candidate)
    return deduped


def looks_like_zip(data: bytes, ctype: str, url: str) -> bool:
    if data.startswith(b"PK\x03\x04") or data.startswith(b"PK\x05\x06"):
        return True
    lower = (ctype + " " + url).lower()
    return "zip" in lower and not data.lstrip().lower().startswith(b"<!doctype html")


def looks_like_html(data: bytes, ctype: str) -> bool:
    return "html" in ctype.lower() or data[:512].lstrip().lower().startswith((b"<!doctype html", b"<html"))


def write_download_as_archive(cache_dir: Path,
                              ref: CoreRef,
                              data: bytes,
                              final_url: str,
                              filename_hint: str | None) -> Path:
    name = safe_name(ref.title) or ref.filehost_id
    if looks_like_zip(data, "", final_url) or allowed_download_ext(filename_hint) == ".zip" or allowed_download_ext(final_url) == ".zip":
        archive = cache_dir / f"{name}-{ref.filehost_id}.zip"
        archive.write_bytes(data)
        return archive

    member = safe_download_filename(filename_hint, final_url, f"{name}.cor")
    archive = cache_dir / f"{name}-{ref.filehost_id}-direct.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(member, data)
    return archive


def dedupe_candidates(candidates: list[DownloadCandidate]) -> list[DownloadCandidate]:
    deduped: list[DownloadCandidate] = []
    seen: set[str] = set()
    for candidate in candidates:
        if candidate.url in seen:
            continue
        seen.add(candidate.url)
        deduped.append(candidate)
    return deduped


def try_download_candidates(ref: CoreRef,
                            cache_dir: Path,
                            cookie: str | None,
                            candidates: list[DownloadCandidate],
                            verbose: bool = False) -> tuple[Path | None, str | None]:
    deduped = dedupe_candidates(candidates)
    for idx, candidate in enumerate(deduped, 1):
        try:
            if verbose:
                label = f" ({candidate.source})" if candidate.source else ""
                print(f"  download candidate {idx}/{len(deduped)}{label}: {candidate.url}", flush=True)
            data, ctype, final_url = fetch_bytes(candidate.url, cookie)
        except Exception as exc:  # noqa: BLE001
            if verbose:
                print(f"    failed: {exc}", flush=True)
            continue

        ext = allowed_download_ext(candidate.filename) or allowed_download_ext(final_url) or allowed_download_ext(candidate.url)
        if not ext:
            if verbose:
                print(f"    skipped: URL/filename has no .zip/.cor/.core/.bit extension", flush=True)
            continue
        if looks_like_html(data, ctype):
            if verbose:
                print(f"    not a core archive/file: {ctype or 'unknown content-type'} {final_url}", flush=True)
            continue
        archive = write_download_as_archive(cache_dir, ref, data, final_url, candidate.filename)
        return archive, final_url
    return None, None


def prefetch_filehost_details(refs: list[CoreRef],
                              cookie: str | None,
                              workers: int,
                              quiet: bool = False) -> dict[str, tuple[list[DownloadCandidate], str | None]]:
    unique_refs: list[CoreRef] = []
    seen: set[str] = set()
    for ref in refs:
        if ref.filehost_id in seen:
            continue
        seen.add(ref.filehost_id)
        unique_refs.append(ref)
    if not unique_refs:
        return {}
    workers = max(1, min(workers, len(unique_refs)))
    if workers <= 1:
        return {
            ref.filehost_id: filehost_detail_candidates(ref.filehost_id, cookie, verbose=False)
            for ref in unique_refs
        }

    if not quiet:
        print(f"Prefetching filehost details for {len(unique_refs)} refs with {workers} workers...", flush=True)
    out: dict[str, tuple[list[DownloadCandidate], str | None]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(filehost_detail_candidates, ref.filehost_id, cookie, False): ref
            for ref in unique_refs
        }
        for fut in concurrent.futures.as_completed(futures):
            ref = futures[fut]
            try:
                out[ref.filehost_id] = fut.result()
            except Exception as exc:  # noqa: BLE001
                out[ref.filehost_id] = ([], f"filehost detail failed: {exc}")
    return out


def download_archive_for_ref(ref: CoreRef,
                             cache_dir: Path,
                             cookie: str | None,
                             verbose: bool = False,
                             detail: tuple[list[DownloadCandidate], str | None] | None = None) -> tuple[Path | None, str | None, str]:
    page_url = f"https://files.mega65.org/?id={ref.filehost_id}"
    if detail is None:
        candidates, detail_status = filehost_detail_candidates(ref.filehost_id, cookie, verbose=verbose)
    else:
        candidates, detail_status = detail
    archive, final_url = try_download_candidates(ref, cache_dir, cookie, candidates, verbose=verbose)
    if archive:
        return archive, final_url, "downloaded"

    page_html = ""
    try:
        if verbose:
            print(f"  catalogue fallback: {page_url}", flush=True)
        page_html = fetch_text(page_url, cookie)
    except Exception as exc:  # noqa: BLE001
        return None, None, detail_status or f"filehost page failed: {exc}"

    archive, final_url = try_download_candidates(
        ref,
        cache_dir,
        cookie,
        candidate_archive_urls(ref.filehost_id, page_html, page_url),
        verbose=verbose,
    )
    if archive:
        return archive, final_url, "downloaded"

    if "Username or email address" in page_html or "Enter the code" in page_html:
        return None, None, "filehost login/code required; set filehost_cookie or filehost_user/password in .m65j.config"
    return None, None, detail_status or "no downloadable .zip/.cor/.core/.bit link found"


# Core selection and extraction.


def safe_name(name: str) -> str:
    name = name.strip().lower()
    name = re.sub(r"[^a-z0-9._-]+", "-", name)
    name = re.sub(r"-+", "-", name).strip("-._")
    return name[:80]


def safe_artifact_filename(name: str) -> str:
    name = name.strip()
    name = re.sub(r"[^A-Za-z0-9._-]+", "-", name)
    name = re.sub(r"-+", "-", name).strip("-._")
    return name[:120]


def canonical_base_name(title: str, member_name: str) -> str:
    candidates = [title, Path(member_name.split("::")[-1]).stem]
    for candidate in candidates:
        name = safe_name(candidate)
        if not name or name == "unknown-core":
            continue
        name = re.sub(r"[._]+", "-", name)
        name = re.sub(r"(?i)(^|-)(mega65)?r[36]($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(rev|revision)-?[36]($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(stable|unstable|nightly|release|released|latest)($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(build|built|b|rel)-?\d+[a-z0-9-]*($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)v(?:er|ersion)?-?\d+(?:-\d+){0,4}[a-z]?($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)\d+(?:-\d+){1,4}($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)20\d{2}-?[01]\d-?[0-3]\d($|-)", "-", name)
        name = re.sub(r"-+", "-", name).strip("-._")
        if name:
            return name[:80]
    return "core"


def canonical_core_filename(title: str, board: str, member_name: str) -> str:
    ext = Path(member_name.split("::")[-1]).suffix.lower()
    if ext == ".core":
        ext = ".cor"
    if ext not in CORE_FILE_EXTS:
        ext = ".cor"
    base = canonical_base_name(title, member_name)
    board_suffix = f"-{board.lower()}"
    if not base.endswith(board_suffix):
        base += board_suffix
    return f"{base}{ext}"


def board_hint_present(text: str, board: str) -> bool:
    return bool(
        re.search(rf"(^|[^a-z0-9]){board}([^a-z0-9]|$)|rev(?:ision)?[_ -]*{board[-1]}", text) or
        f"mega65{board}" in text
    )


def fixed_header_string(data: bytes, off: int, length: int = 32) -> str:
    raw = data[off:off + length].split(b"\x00", 1)[0]
    return raw.decode("ascii", errors="replace").strip()


def cor_header_info(data: bytes) -> tuple[int, str, str, str]:
    if len(data) < 0x71 or data[:16] != COR_MAGIC:
        return 0, "", "", ""
    title = fixed_header_string(data, 0x10)
    version = fixed_header_string(data, 0x30)
    model = fixed_header_string(data, 0x50)
    return data[0x70], title, version, model


def core_board_from_data(path: str, data: bytes) -> tuple[int, str]:
    ext = Path(path.split("::")[-1]).suffix.lower()
    if ext not in (".cor", ".core"):
        return 0, ""
    model_id, _title, _version, model = cor_header_info(data)
    if model_id in (3, 6):
        return model_id, model
    lower_model = model.lower()
    if board_hint_present(lower_model, "r3") and not board_hint_present(lower_model, "r6"):
        return 3, model
    if board_hint_present(lower_model, "r6") and not board_hint_present(lower_model, "r3"):
        return 6, model
    return 0, model


def core_score(path: str, data: bytes, board: str, single: bool) -> int:
    lower = path.lower()
    leaf = Path(path.split("::")[-1]).name.lower()
    other = "r6" if board == "r3" else "r3"
    wanted_id = 3 if board == "r3" else 6
    actual_id, _model = core_board_from_data(path, data)
    score = 0
    if single:
        score += 3

    if actual_id:
        score += 200 if actual_id == wanted_id else -200

    leaf_has_board = board_hint_present(leaf, board)
    leaf_has_other = board_hint_present(leaf, other)
    path_has_board = board_hint_present(lower, board)
    path_has_other = board_hint_present(lower, other)

    if leaf_has_board:
        score += 30
    elif path_has_board and not path_has_other:
        score += 12
    elif path_has_board:
        score += 4

    if leaf_has_other:
        score -= 80
    elif path_has_other and not path_has_board:
        score -= 40

    if "mega65" in lower:
        score += 2
    if lower.endswith(".cor"):
        score += 6
    if lower.endswith(".bit"):
        score += 1
    return score


def core_member_detail(path: str, data: bytes) -> str:
    actual_id, title, version, model = cor_header_info(data)
    parts: list[str] = []
    if title:
        parts.append(f"title={title}")
    if version:
        parts.append(f"version={version}")
    if actual_id:
        parts.append(f"cor_model={board_label(actual_id).upper()}")
    if model:
        parts.append(f"model={model}")
    return " ".join(parts)


def iter_core_members(zf: zipfile.ZipFile, prefix: str = ""):
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = prefix + info.filename
        lower = info.filename.lower()
        if any(lower.endswith(ext) for ext in CORE_FILE_EXTS):
            yield name, zf.read(info)
        elif lower.endswith(".zip"):
            nested_data = zf.read(info)
            try:
                with zipfile.ZipFile(io.BytesIO(nested_data)) as nested:
                    yield from iter_core_members(nested, prefix=name + "::")
            except zipfile.BadZipFile:
                continue


def iter_archive_files(zf: zipfile.ZipFile, prefix: str = ""):
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = prefix + info.filename
        yield name
        if info.filename.lower().endswith(".zip"):
            try:
                with zipfile.ZipFile(io.BytesIO(zf.read(info))) as nested:
                    yield from iter_archive_files(nested, prefix=name + "::")
            except zipfile.BadZipFile:
                continue


def format_archive_file_list(names: list[str], max_entries: int = 120) -> str:
    if not names:
        return "zip contains no regular files"
    shown = names[:max_entries]
    lines = ["zip contains:"]
    lines.extend(f"  {name}" for name in shown)
    if len(names) > len(shown):
        lines.append(f"  ... {len(names) - len(shown)} more")
    return "\n".join(lines)


def has_core_file_ext(path: Path | str) -> bool:
    return Path(str(path)).suffix.lower() in CORE_FILE_EXTS


def iter_extra_core_files(items: list[str]) -> list[Path]:
    found: list[Path] = []
    for item in items or []:
        p = Path(item)
        if not p.exists():
            raise SystemExit(f"extra core does not exist: {p}")
        if p.is_dir():
            found.extend(
                child
                for child in sorted(p.rglob("*"))
                if child.is_file() and has_core_file_ext(child)
            )
        elif p.is_file():
            if not has_core_file_ext(p):
                raise SystemExit(f"extra core is not .bit/.cor/.core/.m65j: {p}")
            found.append(p)
        else:
            raise SystemExit(f"extra core path is not a file or directory: {p}")

    out: list[Path] = []
    seen: set[Path] = set()
    for p in found:
        key = p.resolve()
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out


def extra_core_boards(path: Path, data: bytes, boards: list[str]) -> list[str]:
    payload = strip_signature_trailer(data)
    model_id, _model = core_board_from_data(path.name, payload)
    if model_id in (3, 6):
        wanted = [f"r{model_id}"]
    else:
        lower = path.name.lower()
        has_r3 = board_hint_present(lower, "r3")
        has_r6 = board_hint_present(lower, "r6")
        if has_r3 and not has_r6:
            wanted = ["r3"]
        elif has_r6 and not has_r3:
            wanted = ["r6"]
        else:
            wanted = list(boards)
            if len(boards) > 1:
                print(f"NOTE extra {path}: no board hint; including in R3 and R6 manifests")
    return [b for b in wanted if b in boards]


def copy_extra_core(src: Path,
                    out_dir: Path,
                    board: str,
                    args: argparse.Namespace) -> tuple[Path | None, str]:
    data = src.read_bytes()
    payload = strip_signature_trailer(data)
    _model_id, title, _version, _model = cor_header_info(payload)
    if args.preserve_filenames:
        base = safe_name(src.name)
        if not base or not has_core_file_ext(base):
            base = canonical_core_filename(title or src.stem, board, src.name)
    else:
        base = canonical_core_filename(title or src.stem, board, src.name)

    dest = out_dir / base
    write_data = payload if args.bless else data
    existed_before = dest.exists()
    if dest.exists():
        old_payload = strip_signature_trailer(dest.read_bytes())
        if old_payload == payload:
            return dest, "unchanged"
        if not args.overwrite:
            return dest, "changed; use --overwrite to replace"

    dest.parent.mkdir(parents=True, exist_ok=True)
    if write_data == data:
        shutil.copy2(src, dest)
    else:
        dest.write_bytes(write_data)
    detail = core_member_detail(src.name, payload)
    if detail:
        detail = f" ({detail})"
    return dest, f"{'updated' if existed_before else 'included'} {src}{detail}"


def extract_board_core(archive: Path, out_dir: Path, title: str, board: str, overwrite: bool, preserve_filenames: bool) -> tuple[Path | None, str]:
    try:
        with zipfile.ZipFile(archive) as zf:
            cores = list(iter_core_members(zf))
            archive_files = list(iter_archive_files(zf))
    except zipfile.BadZipFile:
        return None, "bad zip"

    if not cores:
        return None, "no .cor/.bit files in zip\n" + format_archive_file_list(archive_files)

    scored = sorted(
        ((core_score(name, data, board, len(cores) == 1), name, data) for name, data in cores),
        reverse=True,
    )
    score, member_name, data = scored[0]
    if score < 0:
        return None, f"no {board.upper()} .cor/.bit candidate\n" + format_archive_file_list(archive_files)

    if preserve_filenames:
        base = safe_name(Path(member_name.split("::")[-1]).name)
        if not (base.endswith(".bit") or base.endswith(".cor")):
            base += ".cor"
        if not base or base in (".bit", ".cor"):
            base = canonical_core_filename(title, board, member_name)
    else:
        base = canonical_core_filename(title, board, member_name)
    dest = out_dir / base
    existed_before = dest.exists()
    if dest.exists():
        old_payload = strip_signature_trailer(dest.read_bytes())
        if old_payload == data:
            return dest, "unchanged"
        if not overwrite:
            return dest, "changed; use --overwrite to replace"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    detail = core_member_detail(member_name, data)
    if detail:
        detail = f" ({detail})"
    return dest, f"{'updated' if existed_before else 'extracted'} {member_name}{detail}"


def bless_core(path, board, args):
    board_id = "6" if board == "r6" else "3" if board == "r3" else "0"
    cmd = [
        "--bless",
        "--board",
        board_id,
        "--name",
        Path(path).name,
        "-o",
        str(path),
    ]
    if getattr(args, "key", None):
        cmd.extend(["--key", str(args.key)])
    if getattr(args, "key_name", None):
        cmd.extend(["--key-name", str(args.key_name)])
    if getattr(args, "yes", False):
        cmd.append("--yes")
    if getattr(args, "blank_filename", False):
        cmd.append("--blank-filename")
    cmd.append(str(path))
    rc = signing_main(cmd)
    return "blessed" if rc == 0 else f"bless failed: exit {rc}"



def bless_paths(paths: list[Path], board: str, args: argparse.Namespace) -> None:
    paths = sorted(dict.fromkeys(paths))
    if not paths:
        print(f"No {board.upper()} files to bless.")
        return
    for idx, path in enumerate(paths, 1):
        print(f"Blessing {idx}/{len(paths)}: {path}", flush=True)
        status = bless_core(path, board, args)
        print(f"BLESS {path}: {status}")


# Mirror artifacts, manifests, and SD-card bundles.


def copy_artifact(src: str | None, dest: Path, overwrite: bool) -> Path | None:
    if not src:
        return None
    source = Path(src)
    if not source.exists() or not source.is_file():
        raise SystemExit(f"artifact does not exist: {source}")
    if source.resolve() == dest.resolve():
        return dest
    if dest.exists() and not overwrite:
        old_payload = strip_signature_trailer(dest.read_bytes())
        new_payload = strip_signature_trailer(source.read_bytes())
        if old_payload != new_payload:
            raise SystemExit(f"artifact would overwrite changed file; use --overwrite: {dest}")
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
    return dest


def copy_public_firmware(src: str | None, dest: Path, overwrite: bool) -> Path | None:
    if not src:
        return None
    source = Path(src)
    if not source.exists() or not source.is_file():
        raise SystemExit(f"firmware artifact does not exist: {source}")
    data = strip_signature_trailer(source.read_bytes())
    if dest.exists() and dest.read_bytes() == data:
        return dest
    if dest.exists() and not overwrite:
        raise SystemExit(f"plain firmware download would overwrite changed file; use --overwrite: {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return dest


def stage_manifest_artifacts(out_dir: Path, args: argparse.Namespace, boards: list[str]) -> list[ManifestArtifact]:
    artifacts: list[ManifestArtifact] = []
    copy_public_firmware(
        args.firmware,
        out_dir / PUBLIC_FIRMWARE_FILENAME,
        args.overwrite,
    )
    firmware = copy_artifact(
        args.firmware,
        out_dir / OTA_FIRMWARE_FILENAME,
        args.overwrite,
    )
    if firmware:
        artifacts.append(ManifestArtifact(
            kind="firmware",
            path=firmware,
            rel=firmware.relative_to(out_dir).as_posix(),
            version=args.firmware_version or "",
            build=args.firmware_build or "",
        ))

    themes = args.theme or []
    for idx, theme_src in enumerate(themes, 1):
        src_path = Path(theme_src)
        suffix = src_path.suffix.lower()
        if suffix not in (".m65jtheme", ".tar"):
            raise SystemExit(f"theme artifact must be .m65jtheme or .tar: {src_path}")
        dest_name = safe_artifact_filename(src_path.name)
        if not dest_name:
            raise SystemExit(f"cannot derive safe theme filename from: {src_path}")
        theme = copy_artifact(
            theme_src,
            out_dir / "THEMES" / dest_name,
            args.overwrite,
        )
        if theme:
            artifacts.append(ManifestArtifact(
                kind="theme",
                path=theme,
                rel=theme.relative_to(out_dir).as_posix(),
                version=args.theme_version or "",
                name=(args.theme_name if idx == 1 and args.theme_name else src_path.stem),
            ))

    if args.bless:
        board_for_signature = boards[0] if boards else "r6"
        for artifact in artifacts:
            print(f"Blessing {artifact.kind}: {artifact.path}", flush=True)
            status = bless_core(artifact.path, board_for_signature, args)
            print(f"BLESS {artifact.path}: {status}")
    return artifacts


def artifact_attrs(artifact: ManifestArtifact) -> str:
    attrs: list[str] = []
    if artifact.version:
        attrs.append(f"version={artifact.version}")
    if artifact.build:
        attrs.append(f"build={artifact.build}")
    if artifact.name:
        attrs.append(f"name={artifact.name}")
    return (" " + " ".join(attrs)) if attrs else ""


def manifest_row_for_path(out_dir: Path, path: Path, kind: str, rel: str, attrs: str = "") -> str:
    transfer_data = path.read_bytes()
    payload_data = strip_signature_trailer(transfer_data)
    return (
        f"{kind} "
        f"{hashlib.sha256(payload_data).hexdigest()} "
        f"{hashlib.sha256(transfer_data).hexdigest()}  "
        f"{rel}{attrs}\n"
    )


def write_hash_file(out_dir: Path,
                    hash_file: str,
                    board: str,
                    channel: str,
                    paths: list[Path] | None = None,
                    artifacts: list[ManifestArtifact] | None = None) -> Path | None:
    if not hash_file:
        return None

    body_lines: list[str] = []
    for path in sorted(dict.fromkeys(paths if paths is not None else core_file_paths(out_dir))):
        rel = path.relative_to(out_dir).as_posix()
        body_lines.append(manifest_row_for_path(out_dir, path, "core", rel))

    for artifact in artifacts or []:
        body_lines.append(manifest_row_for_path(
            out_dir,
            artifact.path,
            artifact.kind,
            artifact.rel,
            artifact_attrs(artifact),
        ))
    aggregate = hashlib.sha256("".join(body_lines).encode("utf-8")).hexdigest()

    dest = Path(hash_file)
    if not dest.is_absolute():
        dest = out_dir / dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as f:
        f.write("# m65j mirror hash list v3\n")
        f.write(f"# channel={channel}\n")
        f.write(f"# board={board.upper()}\n")
        f.write(f"# aggregate_sha256={aggregate}\n")
        f.write("# format: <type> <payload-sha256> <transfer-sha256>  <relative-filename> [key=value...]\n")
        f.write("# payload-sha256 hashes the SD-stored file after signature stripping\n")
        f.write("# transfer-sha256 hashes the exact HTTP object, including any signature trailer\n")
        for line in body_lines:
            f.write(line)
    return dest


def link_href(path: str) -> str:
    return urllib.parse.quote(path, safe="/._-")


def human_core_name_from_path(path: Path) -> str:
    stem = path.stem
    stem = re.sub(r"[-_.]?r[36]$", "", stem, flags=re.I)
    stem = stem.replace("_", " ").replace("-", " ")
    return " ".join(part for part in stem.split()) or path.name


def mirror_core_info(path: Path) -> tuple[str, str]:
    try:
        with path.open("rb") as f:
            head = f.read(256)
    except OSError:
        return human_core_name_from_path(path), ""
    _model_id, title, version, _model = cor_header_info(head)
    return title or human_core_name_from_path(path), version


def write_mirror_css(out_dir: Path) -> Path:
    css = """\
:root {
  color-scheme: dark;
  --bg: #080b10;
  --panel: #111720;
  --panel-2: #171f2b;
  --line: #2d3a4c;
  --text: #eef3f7;
  --muted: #a7b3c1;
  --accent: #68d391;
  --accent-2: #ffd34d;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background: radial-gradient(circle at top, #1c2734 0, var(--bg) 44rem);
  color: var(--text);
}
a { color: var(--accent); }
.wrap { width: min(1120px, calc(100% - 32px)); margin: 0 auto; padding: 28px 0 56px; }
header { display: flex; align-items: center; justify-content: space-between; gap: 24px; margin-bottom: 28px; }
.logo { width: 320px; max-width: 55vw; height: auto; image-rendering: auto; }
h1 { font-size: clamp(2rem, 5vw, 4rem); margin: 0; line-height: 1; }
h2 { margin: 34px 0 14px; font-size: 1.35rem; }
p { color: var(--muted); line-height: 1.55; }
.hero { border: 1px solid var(--line); background: rgba(17, 23, 32, 0.88); padding: 22px; }
.download { display: inline-block; margin-top: 12px; padding: 12px 16px; background: var(--accent); color: #07100b; text-decoration: none; font-weight: 800; }
.steps { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 14px; }
.step, .note { border: 1px solid var(--line); background: var(--panel); padding: 16px; min-height: 150px; }
.step svg { width: 100%; height: 86px; margin-bottom: 10px; }
.step strong { display: block; margin-bottom: 6px; }
code { color: #ffffff; background: #202a38; padding: 2px 5px; }
table { width: 100%; border-collapse: collapse; background: var(--panel); border: 1px solid var(--line); }
th, td { padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; text-align: left; }
th { color: #ffffff; background: var(--panel-2); }
td.board { min-width: 160px; }
.version { color: var(--accent-2); font-size: 0.9rem; margin-top: 3px; }
.empty { color: #687586; }
ul { color: var(--muted); line-height: 1.65; }
@media (max-width: 760px) {
  header, .steps { display: block; }
  .step { margin-bottom: 12px; }
  table { font-size: 0.92rem; }
  th, td { padding: 8px; }
}
"""
    path = out_dir / "mirror.css"
    path.write_text(css, encoding="utf-8")
    return path


def step_svg(kind: str) -> str:
    if kind == "bootsel":
        return """<svg viewBox="0 0 260 90" aria-hidden="true"><rect x="35" y="18" width="190" height="54" rx="8" fill="#1c2734" stroke="#56657a" stroke-width="3"/><rect x="55" y="30" width="38" height="30" rx="5" fill="#2f3b4d"/><circle cx="174" cy="45" r="15" fill="#68d391"/><text x="130" y="51" fill="#eef3f7" font-size="15" text-anchor="middle">BOOTSEL</text></svg>"""
    if kind == "uf2":
        return """<svg viewBox="0 0 260 90" aria-hidden="true"><rect x="40" y="18" width="66" height="54" rx="6" fill="#273142" stroke="#56657a" stroke-width="3"/><path d="M170 22h38l20 20v30h-58z" fill="#273142" stroke="#ffd34d" stroke-width="3"/><path d="M208 22v20h20" fill="none" stroke="#ffd34d" stroke-width="3"/><path d="M112 45h44" stroke="#68d391" stroke-width="6"/><path d="M146 32l14 13-14 13" fill="none" stroke="#68d391" stroke-width="6"/></svg>"""
    return """<svg viewBox="0 0 260 90" aria-hidden="true"><rect x="60" y="16" width="140" height="58" rx="6" fill="#273142" stroke="#56657a" stroke-width="3"/><rect x="75" y="30" width="110" height="10" fill="#68d391"/><rect x="75" y="49" width="78" height="10" fill="#ffd34d"/><text x="130" y="84" fill="#a7b3c1" font-size="13" text-anchor="middle">FAT SD</text></svg>"""


def zip_add_file(zf: zipfile.ZipFile, source: Path, arcname: str, seen: set[str]) -> None:
    if not source.exists() or not source.is_file():
        return
    arc = arcname.replace("\\", "/").lstrip("/")
    if not arc or arc in seen:
        return
    seen.add(arc)
    zf.write(source, arc)


def zip_add_tree(zf: zipfile.ZipFile, root: Path, prefix: str, seen: set[str]) -> None:
    if not root.exists() or not root.is_dir():
        return
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        zip_add_file(zf, path, f"{prefix.rstrip('/')}/{rel}", seen)


def write_one_sdcard_zip(out_dir: Path,
                         dest: Path,
                         channel_slug: str,
                         boards: list[str],
                         core_paths: list[Path] | None = None,
                         include_manifests: bool = False) -> Path:
    seen: set[str] = set()
    with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        cfg = out_dir / "mega65-jtag.cfg.example"
        zip_add_file(zf, cfg, "mega65-jtag.cfg", seen)
        zip_add_tree(zf, out_dir / "WWW", "WWW", seen)
        zip_add_tree(zf, out_dir / "THEMES", "THEMES", seen)
        for path in sorted(dict.fromkeys(core_paths or [])):
            if path.exists() and path.is_file():
                zip_add_file(zf, path, path.relative_to(out_dir).as_posix(), seen)
        if include_manifests:
            for board in boards:
                manifest = out_dir / f"{channel_slug}-{board}.sha256"
                zip_add_file(zf, manifest, manifest.name, seen)
    return dest


def write_sdcard_zips(out_dir: Path,
                      channel: str,
                      boards: list[str],
                      manifest_paths: dict[str, list[Path]]) -> tuple[Path, dict[str, Path], Path]:
    channel_slug = safe_name(channel) or "stable"
    all_core_paths: list[Path] = []
    for paths in manifest_paths.values():
        all_core_paths.extend(paths)

    minimal = write_one_sdcard_zip(
        out_dir,
        out_dir / SDCARD_BOOTSTRAP_FILENAME,
        channel_slug,
        boards,
        None,
    )
    board_zips: dict[str, Path] = {}
    for board in boards:
        board_zips[board] = write_one_sdcard_zip(
            out_dir,
            out_dir / sdcard_bootstrap_board_cores_filename(board),
            channel_slug,
            [board],
            manifest_paths.get(board, []),
            True,
        )
    all_cores = write_one_sdcard_zip(
        out_dir,
        out_dir / SDCARD_BOOTSTRAP_ALL_CORES_FILENAME,
        channel_slug,
        boards,
        all_core_paths,
        True,
    )
    return minimal, board_zips, all_cores


def write_mirror_index(out_dir: Path,
                       channel: str,
                       boards: list[str],
                       manifest_paths: dict[str, list[Path]],
                       artifacts: list[ManifestArtifact],
                       args: argparse.Namespace) -> Path:
    write_mirror_css(out_dir)
    public_fw = out_dir / PUBLIC_FIRMWARE_FILENAME
    logo = out_dir / "WWW" / "mega65_320x64.png"
    cfg_example = out_dir / "mega65-jtag.cfg.example"
    rows: dict[str, dict[str, tuple[Path, str]]] = {}
    names: dict[str, str] = {}
    for board, paths in manifest_paths.items():
        for path in sorted(dict.fromkeys(paths)):
            title, version = mirror_core_info(path)
            key = re.sub(r"\s+", " ", title).strip().lower() or human_core_name_from_path(path).lower()
            rows.setdefault(key, {})[board] = (path, version)
            names[key] = title or human_core_name_from_path(path)

    board_headers = "".join(f"<th>{html.escape(board.upper())}</th>" for board in boards)
    table_rows: list[str] = []
    for key in sorted(rows, key=lambda k: names.get(k, k).lower()):
        cells = [f"<td>{html.escape(names.get(key, key))}</td>"]
        for board in boards:
            entry = rows[key].get(board)
            if not entry:
                cells.append('<td class="board empty">not offered</td>')
                continue
            path, version = entry
            rel = path.relative_to(out_dir).as_posix()
            label = html.escape(path.name)
            version_html = f'<div class="version">{html.escape(version)}</div>' if version else ""
            cells.append(f'<td class="board"><a href="{link_href(rel)}">{label}</a>{version_html}</td>')
        table_rows.append("<tr>" + "".join(cells) + "</tr>")
    if not table_rows:
        table_rows.append(f'<tr><td colspan="{len(boards) + 1}" class="empty">No cores were staged.</td></tr>')

    artifact_items: list[str] = []
    for artifact in artifacts:
        rel = artifact.path.relative_to(out_dir).as_posix()
        label = artifact.kind
        if artifact.kind == "firmware":
            label = "signed OTA firmware package"
        elif artifact.kind == "theme":
            label = f"theme: {artifact.name or artifact.path.stem}"
        artifact_items.append(f'<li><a href="{link_href(rel)}">{html.escape(rel)}</a> ({html.escape(label)})</li>')
    artifact_html = "\n".join(artifact_items) if artifact_items else "<li>No firmware or theme artifacts staged.</li>"
    channel_slug = safe_name(channel) or "stable"
    manifest_links = "\n".join(
        f'<li><a href="{link_href(f"{channel_slug}-{board}.sha256")}">{html.escape(f"{channel_slug}-{board}.sha256")}</a></li>'
        for board in boards
    )

    logo_html = f'<img class="logo" src="{link_href("WWW/mega65_320x64.png")}" alt="MEGA65">' if logo.exists() else "<h1>MEGA65</h1>"
    fw_link = (f'<a class="download" href="{link_href(PUBLIC_FIRMWARE_FILENAME)}">Download {html.escape(PUBLIC_FIRMWARE_FILENAME)}</a>'
               if public_fw.exists() else '<span class="empty">No plain UF2 firmware image was staged.</span>')
    cfg_line = (f'<a href="{link_href("mega65-jtag.cfg.example")}">mega65-jtag.cfg.example</a>'
                if cfg_example.exists() else '<code>mega65-jtag.cfg</code> from the firmware repository')
    starter_zip = out_dir / SDCARD_BOOTSTRAP_FILENAME
    all_cores_zip = out_dir / SDCARD_BOOTSTRAP_ALL_CORES_FILENAME
    sd_zip_links: list[str] = []
    if starter_zip.exists():
        sd_zip_links.append(
            f'<a class="download" href="{link_href(SDCARD_BOOTSTRAP_FILENAME)}">Download SD starter ZIP</a>'
        )
    for board in boards:
        board_zip_name = sdcard_bootstrap_board_cores_filename(board)
        if (out_dir / board_zip_name).exists():
            sd_zip_links.append(
                f'<a class="download" href="{link_href(board_zip_name)}">Download SD ZIP with {html.escape(board.upper())} cores</a>'
            )
    if all_cores_zip.exists():
        sd_zip_links.append(
            f'<a class="download" href="{link_href(SDCARD_BOOTSTRAP_ALL_CORES_FILENAME)}">Download SD ZIP with all cores</a>'
        )
    sd_zip_html = " ".join(sd_zip_links)
    firmware_version = args.firmware_version or "v0.1"
    firmware_build = args.firmware_build or "unknown build"

    html_text = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MEGA65 Integrated JTAG Mirror</title>
  <link rel="stylesheet" href="mirror.css">
</head>
<body>
  <main class="wrap">
    <header>
      {logo_html}
      <div>
        <h1>Integrated JTAG Mirror</h1>
        <p>Experimental firmware, web theme packages, signed update manifests, and MEGA65 core files.</p>
      </div>
    </header>

    <section class="hero">
      <h2>Firmware</h2>
      <p>Initial install image: {html.escape(firmware_version)} build {html.escape(firmware_build)}. This UF2 is plain, without the m65j signature trailer, for copying to a Pico in BOOTSEL mode.</p>
      {fw_link}
    </section>

    <h2>Install On A Fresh Pico</h2>
    <section class="steps">
      <div class="step">{step_svg("bootsel")}<strong>1. Enter BOOTSEL</strong><p>Hold the Pico BOOTSEL button while plugging in USB. A drive named <code>RPI-RP2</code> should appear.</p></div>
      <div class="step">{step_svg("uf2")}<strong>2. Copy the UF2</strong><p>Copy <code>{html.escape(PUBLIC_FIRMWARE_FILENAME)}</code> to that drive. The Pico reboots into the MEGA65 JTAG firmware.</p></div>
      <div class="step">{step_svg("sd")}<strong>3. Prepare SD</strong><p>Use a FAT formatted SD card. Copy the web files and config below, then edit WiFi and mirror settings.</p></div>
    </section>

    <h2>Fresh SD Card Contents</h2>
    <section class="note">
      <p>{sd_zip_html}</p>
      <ul>
        <li>Copy <code>WWW/</code> to the SD-card root for the default web interface.</li>
        <li>Copy {cfg_line} to the SD-card root as <code>mega65-jtag.cfg</code>, then set <code>ssid</code>, <code>psk</code>, allow rules, and <code>fetch_base_url</code>.</li>
        <li>Copy <code>THEMES/</code> if you want selectable web themes on the device.</li>
        <li><code>{html.escape(SDCARD_BOOTSTRAP_FILENAME)}</code> contains the config, default web files, and theme packages.</li>
        <li><code>sdcard-files-rX-cores.zip</code> contains the same files plus cores and the signed manifest for one board revision.</li>
        <li><code>{html.escape(SDCARD_BOOTSTRAP_ALL_CORES_FILENAME)}</code> contains the same files plus all cores and signed manifests for every board revision in this mirror.</li>
        <li>Either copy the wanted core files and signed manifests now, use the all-cores ZIP, or configure WiFi and run <code>AT+FETCHNOW</code> / "Check for updated cores".</li>
      </ul>
    </section>

    <h2>Signed Manifests</h2>
    <section class="note">
      <ul>
        {manifest_links}
      </ul>
      <p>Devices verify these signed manifests before fetching cores, firmware packages, or themes.</p>
    </section>

    <h2>Artifacts</h2>
    <section class="note">
      <ul>
        {artifact_html}
      </ul>
    </section>

    <h2>Offered Cores</h2>
    <table>
      <thead><tr><th>Core</th>{board_headers}</tr></thead>
      <tbody>
        {"".join(table_rows)}
      </tbody>
    </table>
  </main>
</body>
</html>
"""
    path = out_dir / "index.html"
    path.write_text(html_text, encoding="utf-8")
    return path


# Filehost authentication.


def parse_filehost_config() -> tuple[dict[str, str], Path | None]:
    # Keep the filehost's first-file lookup separate from merged device settings.
    for path in (Path(".m65j.config"), Path.home() / ".m65j.config"):
        try:
            text = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            continue
        cfg: dict[str, str] = {}
        for raw in text.splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            cfg[key.strip().lower()] = value.strip()
        return cfg, path
    return {}, None


def login_filehost(user: str, password: str, cfg_path: Path | None) -> str | None:
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    data = urllib.parse.urlencode({
        "logindata[username]": user,
        "logindata[password]": password,
    }).encode("utf-8")
    req = urllib.request.Request(
        "https://files.mega65.org/php/login.php",
        data=data,
        headers={
            "User-Agent": "mega65-altcore-fetch/1.0",
            "Accept": "application/json, text/javascript, */*; q=0.01",
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
        },
        method="POST",
    )
    try:
        with opener.open(req, timeout=10.0) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        print(f"NOTE filehost login failed: {exc}", file=sys.stderr)
        return None

    try:
        result = json.loads(body)
    except json.JSONDecodeError:
        print("NOTE filehost login did not return JSON; use filehost_cookie instead.", file=sys.stderr)
        return None
    if result.get("state") != "OK":
        msg = result.get("message") or result.get("state") or "not accepted"
        print(f"NOTE filehost login failed: {msg}", file=sys.stderr)
        return None

    cookies = [f"{c.name}={c.value}" for c in jar]
    if not cookies:
        print("NOTE filehost login succeeded but returned no cookies; use filehost_cookie instead.", file=sys.stderr)
        return None
    where = f" from {cfg_path}" if cfg_path else ""
    print(f"Using files.mega65.org login credentials{where}.", file=sys.stderr)
    return "; ".join(cookies)


def load_cookie(args: argparse.Namespace) -> str | None:
    if args.cookie:
        return args.cookie.strip()
    if args.cookie_file:
        return Path(args.cookie_file).read_text(encoding="utf-8").strip()
    cfg, path = parse_filehost_config()
    cookie = cfg.get("filehost_cookie") or cfg.get("mega65_cookie") or cfg.get("files_mega65_cookie")
    if cookie:
        return cookie
    user = cfg.get("filehost_user") or cfg.get("mega65_user") or cfg.get("files_mega65_user")
    password = cfg.get("filehost_password") or cfg.get("mega65_password") or cfg.get("files_mega65_password")
    if user and password:
        return login_filehost(user, password, path)
    if user or password:
        print(f"NOTE {path} has incomplete filehost credentials; set both filehost_user and filehost_password.", file=sys.stderr)
    return None


# Mirror command entry points.


def latest_main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Download MEGA65 alternate-core archives and extract board-specific .cor/.bit files.",
    )
    ap.add_argument("--board", choices=["r3", "r6", "all"], required=True, help="MEGA65 board revision to extract")
    ap.add_argument("--output", default="sdcard/cores", help="directory for extracted .cor/.bit files")
    ap.add_argument("--cache", default=".cache/altcores", help="directory for downloaded zip archives")
    ap.add_argument("--cookie", help="raw Cookie header for files.mega65.org if login is required")
    ap.add_argument("--cookie-file", help="file containing a raw Cookie header")
    ap.add_argument("--source-url", action="append", default=[],
                    help="alternate catalogue URL to scrape; files.mega65.org aliases the default alt-core pages; repeatable")
    ap.add_argument("--keep-zips", action="store_true", help="keep downloaded zip archives in --cache")
    ap.add_argument("--overwrite", action="store_true", help="overwrite existing extracted .bit files")
    ap.add_argument("--dry-run", action="store_true", help="only list discovered filehost IDs")
    ap.add_argument("--limit", type=int, default=0, help="limit number of discovered refs processed")
    ap.add_argument("--manifest", default="", help="write JSON result manifest")
    ap.add_argument("--channel", required=True, help="mirror channel label, e.g. stable, unstable, nightly")
    ap.add_argument("--bless", action="store_true", help="append m65j signed trailer to every extracted/offered core file")
    ap.add_argument("--key", help="explicit P-256 EC private key PEM for --bless")
    ap.add_argument("--key-name", default="default", help="named key under ~/.m65jtag/keys for --bless")
    ap.add_argument("--yes", action="store_true", help="create the selected signing key without prompting")
    ap.add_argument("--blank-filename", action="store_true", help="with --bless, do not bind signatures to filenames")
    ap.add_argument("--hash-file", default=None, help="write per-file hash list; default is <channel>-rX.sha256 in output dir")
    ap.add_argument("--no-hash-file", action="store_true", help="do not write a channel hash list")
    ap.add_argument("--preserve-filenames", action="store_true", help="use archive member filenames instead of canonical <title>-<board> names")
    ap.add_argument("--firmware", help="signed or unsigned firmware artifact to publish under the fixed OTA filename")
    ap.add_argument("--firmware-version", default="", help="firmware version label to advertise in the manifest")
    ap.add_argument("--firmware-build", default="", help="firmware build marker to advertise in the manifest")
    ap.add_argument("--theme", action="append", default=[],
                    help="uncompressed tar theme package to publish under THEMES/; repeatable")
    ap.add_argument("--theme-name", default="theme", help="theme display name to advertise in the manifest")
    ap.add_argument("--theme-version", default="", help="theme version label to advertise in the manifest")
    ap.add_argument("--extra-core", action="append", default=[],
                    help="local .bit/.cor/.core/.m65j file or directory to include in the mirror; repeatable")
    ap.add_argument("--quiet", action="store_true", help="suppress progress chatter")
    ap.add_argument("--detail-workers", type=int, default=8,
                    help="parallel filehost JSON detail fetches before serial downloads; 1 disables")
    args = ap.parse_args(argv)
    return build_mirror(args, ap)


def build_mirror(args: argparse.Namespace, ap: argparse.ArgumentParser) -> int:
    """Build a mirror from parsed latest, mirror, or populate options."""
    if args.board == "all" and args.hash_file:
        ap.error("--hash-file cannot be used with --board all; manifests are <channel>-r3.sha256 and <channel>-r6.sha256")

    cookie = load_cookie(args)
    refs = scrape_altcore_refs(cookie, args.source_url or None)
    if args.limit:
        refs = refs[: args.limit]

    board = args.board.lower()
    boards = ["r3", "r6"] if board == "all" else [board]
    wanted = [
        ref for ref in refs
        if board == "all" or ref.board_hint in ("unknown", "both", board)
    ]

    board_label_text = "R3/R6" if board == "all" else board.upper()
    print(f"Discovered {len(refs)} filehost refs, {len(wanted)} applicable to {board_label_text}.")
    for ref in wanted:
        print(f"{ref.filehost_id} {ref.board_hint:7} {ref.title}")
    if args.dry_run:
        return 0

    out_dir = Path(args.output)
    cache_root = Path(args.cache)
    cache_root.mkdir(parents=True, exist_ok=True)
    tmp_ctx = tempfile.TemporaryDirectory(prefix="altcores-") if not args.keep_zips else None
    cache_dir = Path(tmp_ctx.name) if tmp_ctx else cache_root
    cache_dir.mkdir(parents=True, exist_ok=True)

    results: list[DownloadResult] = []
    manifest_paths: dict[str, list[Path]] = {b: [] for b in boards}
    detail_cache = prefetch_filehost_details(wanted, cookie, args.detail_workers, quiet=args.quiet)
    try:
        total = len(wanted)
        for idx, ref in enumerate(wanted, 1):
            if not args.quiet:
                print(f"\n[{idx}/{total}] {ref.title} ({ref.filehost_id}, hint={ref.board_hint})", flush=True)
            archive, archive_url, status = download_archive_for_ref(
                ref,
                cache_dir,
                cookie,
                verbose=not args.quiet,
                detail=detail_cache.get(ref.filehost_id),
            )
            if not archive:
                print(f"SKIP {ref.title}: {status}")
                results.append(DownloadResult(ref.title, ref.filehost_id, archive_url, None, status))
                continue
            if not args.quiet:
                print(f"  archive: {archive}", flush=True)
            if args.keep_zips and archive.parent != cache_root:
                shutil.copy2(archive, cache_root / archive.name)
            for one_board in boards:
                if ref.board_hint not in ("unknown", "both", one_board):
                    continue
                dest, extract_status = extract_board_core(archive, out_dir, ref.title, one_board, args.overwrite, args.preserve_filenames)
                label = f"{ref.title} [{one_board.upper()}]" if board == "all" else ref.title
                print(f"{'OK' if dest else 'SKIP'} {label}: {extract_status}")
                if dest:
                    manifest_paths[one_board].append(dest)
                results.append(DownloadResult(label, ref.filehost_id, archive_url, str(dest) if dest else None, extract_status))
    finally:
        if tmp_ctx:
            tmp_ctx.cleanup()

    for extra in iter_extra_core_files(args.extra_core):
        data = extra.read_bytes()
        extra_boards = extra_core_boards(extra, data, boards)
        if not extra_boards:
            print(f"SKIP extra {extra}: board does not match {board_label_text}")
            results.append(DownloadResult(f"extra {extra.name}", "local-extra", str(extra), None, "board does not match"))
            continue
        for one_board in extra_boards:
            dest, extra_status = copy_extra_core(extra, out_dir, one_board, args)
            label = f"{extra.name} [{one_board.upper()}]" if board == "all" else extra.name
            print(f"{'OK' if dest else 'SKIP'} extra {label}: {extra_status}")
            if dest:
                manifest_paths[one_board].append(dest)
            results.append(DownloadResult(label, "local-extra", str(extra), str(dest) if dest else None, extra_status))

    if args.manifest:
        Path(args.manifest).write_text(json.dumps([asdict(r) for r in results], indent=2), encoding="utf-8")
    if args.bless:
        for one_board in boards:
            bless_paths(manifest_paths[one_board], one_board, args)
    artifacts = stage_manifest_artifacts(out_dir, args, boards)
    if not args.no_hash_file:
        for one_board in boards:
            hash_name = args.hash_file or f"{safe_name(args.channel) or 'stable'}-{one_board}.sha256"
            hash_path = write_hash_file(out_dir, hash_name, one_board, args.channel, manifest_paths[one_board], artifacts)
            if hash_path:
                print(f"Wrote hash file: {hash_path} ({len(manifest_paths[one_board])} cores, {len(artifacts)} artifacts)")
                if args.bless:
                    status = bless_core(hash_path, one_board, args)
                    print(f"BLESS {hash_path}: {status}")
    sd_zip, sd_board_zips, sd_all_zip = write_sdcard_zips(out_dir, args.channel, boards, manifest_paths)
    print(f"Wrote SD card ZIP: {sd_zip}")
    for one_board in boards:
        if one_board in sd_board_zips:
            print(f"Wrote SD card {one_board.upper()} cores ZIP: {sd_board_zips[one_board]}")
    print(f"Wrote SD card all-cores ZIP: {sd_all_zip}")
    index_path = write_mirror_index(out_dir, args.channel, boards, manifest_paths, artifacts, args)
    print(f"Wrote mirror index: {index_path}")
    for one_board in boards:
        print(f"SUMMARY {one_board.upper()}: manifest_core_entries={len(manifest_paths[one_board])} artifacts={len(artifacts)}")
    return 0


def normalize_board_for_mirror(board):
    b = str(board).lower()
    if b in {"3", "r3"}:
        return "r3"
    if b in {"6", "r6"}:
        return "r6"
    if b in {"all", "both"}:
        return "all"
    raise SystemExit("board must be r3, r6, 3, 6, or all")


def mirror_boards(board):
    b = normalize_board_for_mirror(board)
    return ["r3", "r6"] if b == "all" else [b]


def board_id_for_mirror(board):
    b = normalize_board_for_mirror(board)
    if b == "r3":
        return "3"
    if b == "r6":
        return "6"
    return "0"


def safe_channel_name(name):
    out = "".join(c.lower() if c.isalnum() else "-" for c in str(name))
    out = "-".join(part for part in out.split("-") if part)
    return out or "stable"


def looks_like_http_url(value):
    lower = str(value).lower()
    return lower.startswith("http://") or lower.startswith("https://")


def resolve_mirror_positionals(ap, items):
    known_release_tags = {
        "stable",
        "unstable",
        "nightly",
        "latest",
        "release",
        "testing",
        "dev",
        "devel",
    }
    if not items:
        ap.error("mirror expects: [release-type] <output-dir> [source-url...]")

    if len(items) == 1:
        if looks_like_http_url(items[0]):
            ap.error("missing output directory before source URL; try: mirror stable mirror https://...")
        return "stable", Path(items[0]), []

    if looks_like_http_url(items[1]):
        if items[0].lower() in known_release_tags:
            ap.error("missing output directory before source URL; try: mirror stable mirror https://...")
        return "stable", Path(items[0]), items[1:]

    if looks_like_http_url(items[0]):
        ap.error("source URL must come after the output directory; try: mirror stable mirror https://...")

    release_type = items[0]
    output = Path(items[1])
    source_urls = items[2:]
    if looks_like_http_url(str(output)):
        ap.error("output directory looks like a URL; try: mirror stable mirror https://...")
    return release_type, output, source_urls


def add_mirror_options(ap, populate=False):
    ap.add_argument("--board", choices=("r3", "r6", "3", "6", "all"), required=True, help="MEGA65 board revision, or all")
    ap.add_argument("--source-url", action="append", default=[],
                    help="alternate catalogue URL to scrape; files.mega65.org aliases the default alt-core pages; repeatable")
    ap.add_argument("--cache", default=".cache/altcores", help="directory for downloaded zip archives")
    ap.add_argument("--cookie", help="raw Cookie header for files.mega65.org if login is required")
    ap.add_argument("--cookie-file", help="file containing a raw Cookie header")
    ap.add_argument("--keep-zips", action="store_true", help="keep downloaded zip archives in --cache")
    ap.add_argument("--overwrite", action="store_true", help="replace changed canonical core files")
    ap.add_argument("--limit", type=int, default=0, help="limit discovered refs processed")
    ap.add_argument("--manifest", default="", help="write JSON result manifest")
    ap.add_argument("--key", help="explicit P-256 EC private key PEM for blessing")
    ap.add_argument("--key-name", default=DEFAULT_KEY_NAME, help="named key under ~/.m65jtag/keys")
    ap.add_argument("--yes", action="store_true", help="create the selected signing key without prompting")
    ap.add_argument("--blank-filename", action="store_true", help="do not bind signatures to filenames")
    ap.add_argument("--hash-file", default=None, help="hash-list filename; default is <release-type>-rX.sha256")
    ap.add_argument("--no-hash-file", action="store_true", help="do not write a release hash list")
    ap.add_argument("--preserve-filenames", action="store_true", help="use archive member filenames instead of canonical names")
    ap.add_argument("--firmware", help="firmware UF2/bin package to publish under the fixed OTA filename")
    ap.add_argument("--firmware-version", default="", help="firmware version label for the mirror manifest")
    ap.add_argument("--firmware-build", default="", help="firmware build marker for the mirror manifest")
    ap.add_argument("--theme", action="append", default=[],
                    help="uncompressed tar theme package to publish under THEMES/; repeatable")
    ap.add_argument("--theme-name", default="theme", help="theme display name for the mirror manifest")
    ap.add_argument("--theme-version", default="", help="theme version label for the mirror manifest")
    ap.add_argument("--extra-core", action="append", default=[],
                    help="local .bit/.cor/.core/.m65j file or directory to include in the mirror; repeatable")
    ap.add_argument("--quiet", action="store_true", help="suppress mirror progress chatter")
    ap.add_argument("--detail-workers", type=int, default=8, help="parallel filehost JSON detail fetches; 1 disables")
    if populate:
        ap.add_argument("--device", "--url", "-u", dest="device", help="base board URL, e.g. http://mega65-jtag.local")
        ap.add_argument("--staging", type=Path, help="local staging directory; defaults to a temporary directory")
        ap.add_argument("--no-bless", action="store_true", help="upload files without signing/blessing them first")
        ap.add_argument("--user", help="HTTP Basic auth user")
        ap.add_argument("--password", help="HTTP Basic auth password")
    else:
        ap.add_argument("--bless", action="store_true", help="append signed trailers to offered core files")
        ap.add_argument("--dry-run", action="store_true", help="only list discovered filehost IDs")


def mirror_main(argv):
    ap = argparse.ArgumentParser(
        prog="m65j.py mirror",
        description="Build a local canonical MEGA65 core mirror for a release channel.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent(
            """\
            Positional order:
              m65j.py mirror [options] <release-type> <output-dir> [source-url...]
              m65j.py mirror [options] <output-dir> [source-url...]

            If <release-type> is omitted, it defaults to stable. Source URLs
            always come after the output directory.

            Examples:
              m65j.py mirror --board all mirror https://files.mega65.org
              m65j.py mirror --board r6 stable mirror https://files.mega65.org
              m65j.py mirror --board r6 stable sdcard/cores --overwrite --bless --yes
              m65j.py mirror --board all stable mirror --extra-core extra_cores
            """
        ),
    )
    ap.add_argument("items", nargs="*", metavar="ARGS",
                    help="positionals; see positional order below")
    add_mirror_options(ap, populate=False)
    ns = ap.parse_args(argv)
    release_type, output, source_urls = resolve_mirror_positionals(ap, ns.items)
    ns.board = normalize_board_for_mirror(ns.board)
    ns.output = output
    ns.channel = release_type
    ns.source_url.extend(source_urls)
    return build_mirror(ns, ap)


def read_hash_manifest(path):
    rows = []
    data = Path(path).read_bytes()
    if len(data) >= TRAILER_LEN and data[-TRAILER_LEN:-TRAILER_LEN + len(MAGIC)] == MAGIC:
        data = data[:-TRAILER_LEN]
    for line_no, raw in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        kind = "core"
        if len(parts) >= 4 and parts[0].lower() in {"core", "firmware", "fw", "theme", "www-theme"}:
            kind = "firmware" if parts[0].lower() == "fw" else ("theme" if parts[0].lower() == "www-theme" else parts[0].lower())
            payload_sha = parts[1].lower()
            transfer_sha = parts[2].lower()
            rel = parts[3].strip()
        elif len(parts) >= 3:
            payload_sha = parts[0].lower()
            transfer_sha = parts[1].lower()
            rel = parts[2].strip()
        else:
            raise SystemExit(f"bad hash manifest line {line_no}: {raw}")
        if len(payload_sha) != 64 or len(transfer_sha) != 64:
            raise SystemExit(f"bad hash manifest line {line_no}: {raw}")
        if any(c not in "0123456789abcdef" for c in payload_sha):
            raise SystemExit(f"bad payload SHA-256 on manifest line {line_no}")
        if transfer_sha and any(c not in "0123456789abcdef" for c in transfer_sha):
            raise SystemExit(f"bad transfer SHA-256 on manifest line {line_no}")
        if not rel or rel.startswith("/") or ".." in rel or "\\" in rel or ":" in rel:
            raise SystemExit(f"unsafe manifest filename on line {line_no}: {rel}")
        rows.append((kind, payload_sha, transfer_sha, rel))
    return rows


def populate_main(argv):
    ap = argparse.ArgumentParser(
        prog="m65j.py populate",
        description="Mirror a release channel and upload the resulting files to the board SD card over HTTP.",
    )
    ap.add_argument("release_type", help="release channel tag, e.g. stable, unstable, nightly")
    ap.add_argument("source_urls", nargs="*", help="alternate catalogue URL(s) to scrape instead of the defaults")
    add_mirror_options(ap, populate=True)
    ns = ap.parse_args(argv)

    device = require_device(ns.device)
    user, password = config_auth(ns.user, ns.password)
    staging_ctx = None
    if ns.staging:
        out_dir = ns.staging
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        staging_ctx = tempfile.TemporaryDirectory(prefix="m65j-populate-")
        out_dir = Path(staging_ctx.name)

    try:
        bless = not ns.no_bless
        ns.board = normalize_board_for_mirror(ns.board)
        ns.output = out_dir
        ns.channel = ns.release_type
        ns.source_url.extend(ns.source_urls)
        ns.bless = bless
        ns.dry_run = False
        rc = build_mirror(ns, ap)
        if rc != 0:
            return rc

        hash_names = []
        if not ns.no_hash_file:
            if ns.hash_file and normalize_board_for_mirror(ns.board) == "all":
                raise SystemExit("populate --board all uses <release>-r3.sha256 and <release>-r6.sha256; omit --hash-file")
            for board in mirror_boards(ns.board):
                hash_names.append(ns.hash_file or f"{safe_channel_name(ns.release_type)}-{board}.sha256")
        if not hash_names:
            raise SystemExit("populate requires a hash file; omit --no-hash-file")

        uploaded: set[str] = set()
        hash_paths: list[tuple[str, Path]] = []
        for board, hash_name in zip(mirror_boards(ns.board), hash_names):
            hash_path = Path(hash_name)
            if not hash_path.is_absolute():
                hash_path = out_dir / hash_path
            hash_paths.append((board, hash_path))
            rows = read_hash_manifest(hash_path)
            for kind, _payload_sha, _transfer_sha, rel in rows:
                if kind != "core":
                    print(f"SKIP populate artifact row kind={kind} file={rel}; device-side fetch handles this", file=sys.stderr)
                    continue
                local = out_dir / rel
                if not local.exists():
                    raise SystemExit(f"manifest entry is missing locally: {local}")
                if rel in uploaded:
                    continue
                uploaded.add(rel)
                put_url = device_put_url(device, rel, 0, True)
                print(f"PUT {put_url}", file=sys.stderr)
                put_file(put_url, local.read_bytes(), user, password)

        # Upload the release hash list last, signed if populate is doing normal
        # blessed uploads. The firmware stores only the payload after verifying.
        for board, hash_path in hash_paths:
            if bless:
                rc = signing_main([
                    "--device", device,
                    "--store-only",
                    "--board", board_id_for_mirror(board),
                    "--name", hash_path.name,
                    str(hash_path),
                ])
                if rc != 0:
                    return rc
            else:
                put_url = device_put_url(device, hash_path.name, 0, True)
                print(f"PUT {put_url}", file=sys.stderr)
                put_file(put_url, hash_path.read_bytes(), user, password)
        return 0
    finally:
        if staging_ctx:
            staging_ctx.cleanup()


def first_arg_is_device(args):
    if not args:
        return False
    value = args[0]
    lower = value.lower()
    if Path(value).exists() or lower.endswith((".bit", ".cor", ".m65j")):
        return False
    if "://" in value or value.startswith("[") or lower == "localhost":
        return True
    parts = value.split(".")
    if len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        return True
    return "." in value and "/" not in value and "\\" not in value


def known_top_level_command(value):
    lower = str(value).lower()
    upper = str(value).upper()
    if upper.startswith("AT") or upper == "GO64":
        return True
    if upper in {
        "?", "V", "L", "LIST", "DIR", "LS", "LL", "DETAIL", "COREDETAIL",
        "CORELS", "I", "T", "P", "LOAD", "PROGRAM", "JTAGLOAD", "S", "N",
        "W", "F", "R", "A", "D", "J", "X", "H", "M", "STATUS", "VERSION",
        "VER", "IDENTIFY", "ABOUT", "WIFI", "HTTP", "SDCARD", "SDSTATUS",
        "DOWNLOADSTATUS", "DLSTATUS", "FWSTATUS", "FIRMWARESTATUS",
        "THEMESTATUS", "WEBTHEMESTATUS",
        "MACHINE", "MACHINENAME", "IDENTITY",
    }:
        return True
    return lower in {
        "web", "net", "tcp", "status", "index", "home", "list", "ls", "cores",
        "load", "program", "jtag-file", "jtagload", "get", "download",
        "file-get", "downloads-get", "read-download", "download-get",
        "mirror", "make-mirror", "populate", "populate-sd", "install-mirror",
        "latest", "fetch-latest", "update-cores", "altcores", "keys",
        "check", "verify", "blessedness", "curse-check", "bless", "sign",
        "push", "jtag", "jtag-push", "store", "store-file", "http-store",
        "put", "http-put", "signing", "stream", "push-local", "program-local",
        "write", "upload", "install-file", "sink", "dummy", "rx-test",
        "machine", "machines", "fwstatus", "firmwarestatus", "themestatus",
        "webthemestatus",
    }


def first_arg_is_target(args):
    if not args:
        return False
    value = args[0]
    if is_serial_port_arg(value) or is_web_target_arg(value):
        return True
    if looks_like_machine_target(value) and not known_top_level_command(value):
        return True
    return False


def is_serial_port_arg(value):
    lower = str(value).lower()
    return (
        lower.startswith("/dev/tty") or
        lower.startswith("/dev/cu") or
        lower.startswith("/dev/serial/") or
        re.match(r"^com\d+$", lower) is not None
    )


def parse_leading_global_options(argv):
    opts = {
        "serial": None,
        "url": None,
        "baud": None,
        "timeout": None,
        "monitor": False,
        "monitor_for": None,
        "list": False,
    }
    rest = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--":
            rest.extend(argv[i + 1:])
            break

        if arg in {"-l", "--list-machines"}:
            opts["list"] = True
            i += 1
            continue

        if arg in {"-m", "--monitor", "--follow"}:
            opts["monitor"] = True
            i += 1
            continue
        if arg == "--monitor-for":
            if i + 1 >= len(argv):
                raise SystemExit("--monitor-for expects seconds")
            opts["monitor"] = True
            opts["monitor_for"] = float(argv[i + 1])
            i += 2
            continue
        if arg.startswith("--monitor-for="):
            opts["monitor"] = True
            opts["monitor_for"] = float(arg.split("=", 1)[1])
            i += 1
            continue

        if arg in {"-s", "--device", "--serial", "--tty"}:
            if i + 1 >= len(argv):
                raise SystemExit(f"{arg} expects a serial device path")
            opts["serial"] = argv[i + 1]
            i += 2
            continue
        if arg.startswith("--device=") or arg.startswith("--serial=") or arg.startswith("--tty="):
            opts["serial"] = arg.split("=", 1)[1]
            i += 1
            continue

        if arg in {"-u", "--url", "--web-url"}:
            if i + 1 >= len(argv):
                raise SystemExit(f"{arg} expects a board HTTP URL")
            opts["url"] = argv[i + 1]
            i += 2
            continue
        if arg.startswith("--url=") or arg.startswith("--web-url="):
            opts["url"] = arg.split("=", 1)[1]
            i += 1
            continue

        if arg == "--baud":
            if i + 1 >= len(argv):
                raise SystemExit("--baud expects a rate")
            opts["baud"] = int(argv[i + 1], 0)
            i += 2
            continue
        if arg.startswith("--baud="):
            opts["baud"] = int(arg.split("=", 1)[1], 0)
            i += 1
            continue

        if arg == "--timeout":
            if i + 1 >= len(argv):
                raise SystemExit("--timeout expects seconds")
            opts["timeout"] = float(argv[i + 1])
            i += 2
            continue
        if arg.startswith("--timeout="):
            opts["timeout"] = float(arg.split("=", 1)[1])
            i += 1
            continue

        rest.extend(argv[i:])
        break
    return opts, rest


def rewrite_command_first_serial(argv):
    if len(argv) < 2 or not is_serial_port_arg(argv[1]):
        return argv
    verb = argv[0].lower()
    if verb in {
        "load", "program", "jtagload",
        "push", "jtag", "jtag-push",
        "stream", "push-local", "program-local",
        "store", "write", "upload", "install-file",
        "sink", "dummy", "rx-test",
    }:
        return [argv[1], argv[0], *argv[2:]]
    return argv


def rewrite_command_first_target(argv):
    argv = rewrite_command_first_serial(argv)
    if len(argv) < 2:
        return argv
    verb = argv[0].lower()
    if verb in {
        "load", "program", "jtagload",
        "push", "jtag", "jtag-push",
        "stream", "push-local", "program-local",
        "store", "write", "upload", "install-file",
        "sink", "dummy", "rx-test",
        "status", "index", "home", "list", "ls", "cores",
        "get", "download", "file-get", "downloads-get", "read-download",
        "download-get",
    } and first_arg_is_target([argv[1]]):
        return [argv[1], argv[0], *argv[2:]]
    return argv


def serial_command_candidate(argv):
    if not argv:
        return False
    first = argv[0]
    upper = first.upper()
    lower = first.lower()
    if upper.startswith("AT") or upper == "GO64":
        return True
    if upper in {
        "?", "V", "L", "LIST", "DIR", "LS", "LL", "DETAIL", "COREDETAIL",
        "CORELS", "I", "T", "P", "LOAD", "PROGRAM", "JTAGLOAD", "S", "N",
        "W", "F", "R", "A", "D", "J", "X", "H", "M", "STATUS", "VERSION",
        "VER", "IDENTIFY", "ABOUT", "WIFI", "HTTP", "SDCARD", "SDSTATUS",
        "DOWNLOADSTATUS", "DLSTATUS", "FWSTATUS", "FIRMWARESTATUS",
        "THEMESTATUS", "WEBTHEMESTATUS",
        "MACHINE", "MACHINENAME", "IDENTITY",
    }:
        return True
    return lower in {
        "stream", "push", "jtag", "jtag-push", "push-local", "program-local",
        "store", "write", "upload", "install-file", "sink", "dummy", "rx-test",
    }


def serial_preferred_even_with_web_config(argv):
    if not argv:
        return False
    first = argv[0]
    upper = first.upper()
    lower = first.lower()
    if upper.startswith("AT") or upper == "GO64":
        return True
    if upper in {
        "?", "V", "L", "LIST", "DIR", "LS", "LL", "DETAIL", "COREDETAIL",
        "CORELS", "I", "T", "P", "LOAD", "PROGRAM", "JTAGLOAD", "S", "N", "W", "F", "R", "A", "D",
        "J", "X", "H", "M", "VERSION", "VER", "IDENTIFY", "ABOUT",
        "WIFI", "HTTP", "SDCARD", "SDSTATUS", "DOWNLOADSTATUS", "DLSTATUS",
        "FWSTATUS", "FIRMWARESTATUS", "THEMESTATUS", "WEBTHEMESTATUS",
        "MACHINE", "MACHINENAME", "IDENTITY",
    }:
        return True
    return lower in {"stream", "sink", "dummy", "rx-test", "push-local", "program-local"}


def resolve_implicit_serial_port(argv, baud, explicit_url=False):
    if explicit_url or not serial_command_candidate(argv):
        return None
    serial_port = configured_serial_port()
    web_device = configured_device()
    if serial_port:
        if web_device is None or serial_preferred_even_with_web_config(argv):
            return serial_port
        return None
    if web_device is None:
        return autodetect_serial_port(baud)
    return None


def pop_optional_device(args):
    if first_arg_is_device(args):
        return args[0], args[1:]
    return None, args


def route_web_command(verb, rest):
    if verb in {"identity", "machine", "name"}:
        device, rest = pop_optional_device(rest)
        ap = argparse.ArgumentParser(prog=f"m65j.py {verb}")
        ap.add_argument("--user")
        ap.add_argument("--password")
        args = ap.parse_args(rest)
        user, password = config_auth(args.user, args.password)
        body, _ = http_request(web_url(device, "/identity"), user, password)
        print(body.decode("utf-8", "replace"), end="")
        return 0

    if verb in {"status", "index", "home", "list", "ls", "cores"}:
        ap = argparse.ArgumentParser(prog=f"m65j.py {verb}")
        ap.add_argument("device", nargs="?")
        ap.add_argument("--board", choices=("3", "6"))
        ap.add_argument("--user")
        ap.add_argument("--password")
        args = ap.parse_args(rest)
        user, password = config_auth(args.user, args.password)
        return web_status(args.device, args.board, user, password)

    if verb in {"load", "program", "jtag-file", "jtagload"}:
        device, rest = pop_optional_device(rest)
        ap = argparse.ArgumentParser(prog=f"m65j.py {verb}")
        ap.add_argument("file", help="existing SD-card core path")
        ap.add_argument("--board", choices=("3", "6"))
        ap.add_argument("--user")
        ap.add_argument("--password")
        args = ap.parse_args(rest)
        user, password = config_auth(args.user, args.password)
        return web_jtag_file(device, args.file, args.board, user, password)

    if verb in {"get", "download", "file-get"}:
        device, rest = pop_optional_device(rest)
        ap = argparse.ArgumentParser(prog=f"m65j.py {verb}")
        ap.add_argument("file", help="SD-card core path under /files")
        ap.add_argument("-o", "--output")
        ap.add_argument("--user")
        ap.add_argument("--password")
        args = ap.parse_args(rest)
        user, password = config_auth(args.user, args.password)
        return web_file_get(device, args.file, args.output, user, password, downloads=False)

    if verb in {"downloads-get", "read-download", "download-get"}:
        device, rest = pop_optional_device(rest)
        ap = argparse.ArgumentParser(prog=f"m65j.py {verb}")
        ap.add_argument("name", help="file name under DOWNLOADS")
        ap.add_argument("-o", "--output")
        ap.add_argument("--user")
        ap.add_argument("--password")
        args = ap.parse_args(rest)
        user, password = config_auth(args.user, args.password)
        return web_file_get(device, args.name, args.output, user, password, downloads=True)

    return None


def route_at_over_web(argv):
    cmd = " ".join(argv).strip()
    upper = cmd.upper()
    if upper.startswith("AT+MACHINE") or upper.startswith("AT+IDENTITY"):
        body, _ = http_request(web_url(None, "/identity"))
        print(body.decode("utf-8", "replace"), end="")
        return 0
    if upper in {"AT", "ATI"} or upper.startswith(("AT+VERSION", "AT+CORELIST", "AT+HELP", "AT+WRITEGRANT", "AT+REMOTE")):
        return web_status()
    if upper.startswith("AT+JTAGLOAD="):
        return web_jtag_file(None, cmd.split("=", 1)[1].strip())
    if upper.startswith("AT+DOWNLOADREAD="):
        return web_file_get(None, cmd.split("=", 1)[1].strip(), None, downloads=True)
    if upper.startswith("AT+FETCH="):
        raise SystemExit("AT+FETCH is serial-only for now; use web push/store/get commands from the host side.")
    raise SystemExit(f"{cmd} requires a serial port; use a web command such as status, load, get, push, or store.")


def route_remote_command(argv):
    if not argv:
        return route_web_command("status", [])

    verb = argv[0].lower()
    rest = argv[1:]

    if verb in {"web", "net", "tcp"}:
        if not rest:
            return route_web_command("status", [])
        routed = route_web_command(rest[0].lower(), rest[1:])
        if routed is not None:
            return routed
        return route_remote_command(rest)

    if verb in {"machines", "list-machines", "discover"}:
        return list_available_machines(2_000_000)

    routed = route_web_command(verb, rest)
    if routed is not None:
        return routed

    if verb.startswith("at") or verb == "go64":
        return route_at_over_web(argv)

    if verb in {"mirror", "make-mirror"}:
        return mirror_main(rest)

    if verb in {"populate", "populate-sd", "install-mirror"}:
        return populate_main(rest)

    if verb in {"latest", "fetch-latest", "update-cores", "altcores"}:
        return latest_main(rest)

    if verb in {"keys", "--keys"}:
        return signing_main(["--keys", *rest])

    if verb in {"check", "verify", "blessedness", "curse-check"}:
        return check_main(rest)

    if verb in {"bless", "sign"}:
        if "--bless" not in rest:
            rest = ["--bless", *rest]
        return signing_main(rest)

    if verb in {"push", "jtag", "jtag-push"}:
        if "--device" in rest or "--put" in rest:
            return signing_main(rest)
        device, rest = pop_optional_device(rest)
        if len(rest) < 1:
            raise SystemExit("push expects: push [device-url] <core.bit|core.cor> [bless options]")
        input_path, *extra = rest
        device = require_device(device)
        return signing_main(["--device", device, *extra, input_path])

    if verb in {"store", "store-file", "http-store"}:
        if "--device" in rest:
            return signing_main(["--store-only", *rest])
        device, rest = pop_optional_device(rest)
        if len(rest) < 1:
            raise SystemExit("store expects: store [device-url] <core.bit|core.cor> [bless options]")
        input_path, *extra = rest
        device = require_device(device)
        return signing_main(["--device", device, "--store-only", *extra, input_path])

    if verb in {"put", "http-put"}:
        if "--put" in rest:
            return signing_main(rest)
        if len(rest) < 2:
            raise SystemExit("put expects: put <exact-put-url> <core.bit|core.cor> [bless options]")
        url, input_path, *extra = rest
        return signing_main(["--put", url, *extra, input_path])

    if verb == "signing":
        return signing_main(rest)

    return None


def command_can_use_default_target(argv):
    if not argv:
        return True
    verb = argv[0].lower()
    if serial_command_candidate(argv):
        return True
    return verb in {
        "web", "net", "tcp", "status", "index", "home", "list", "ls", "cores",
        "load", "program", "jtag-file", "jtagload", "get", "download",
        "file-get", "downloads-get", "read-download", "download-get",
        "push", "jtag", "jtag-push", "store", "store-file", "http-store",
        "put", "http-put", "populate", "populate-sd", "install-mirror",
    }


def read_exact(f, n):
    b = f.read(n)
    if len(b) != n:
        raise ValueError("short read while parsing bitstream")
    return b


def be16(b):
    return (b[0] << 8) | b[1]


def be32(b):
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def parse_xilinx_bit_at(path, base):
    with open(path, "rb") as f:
        f.seek(base)
        n = be16(read_exact(f, 2))
        if n == 0 or n > 4096:
            raise ValueError("not a recognised Xilinx .bit wrapper")
        f.seek(n, os.SEEK_CUR)

        marker = be16(read_exact(f, 2))
        if marker != 1:
            raise ValueError(f"bad .bit marker after magic: 0x{marker:04x}")

        for _ in range(32):
            tag = read_exact(f, 1)[0]
            if ord('a') <= tag <= ord('d'):
                slen = be16(read_exact(f, 2))
                f.seek(slen, os.SEEK_CUR)
            elif tag == ord('e'):
                payload_len = be32(read_exact(f, 4))
                payload_off = f.tell()
                expected = 0
                if payload_len > 0x84:
                    f.seek(payload_off + 0x80)
                    expected = be32(read_exact(f, 4))
                # 0xffffffff is commonly a Xilinx dummy word, not a device ID.
                if expected == 0xffffffff:
                    expected = 0
                return payload_off, payload_len, expected
            else:
                raise ValueError(f"unknown .bit field tag before payload: 0x{tag:02x}")

    raise ValueError(".bit payload field not found")


def inspect_local_core(path):
    path = os.path.expanduser(path)
    with open(path, "rb") as f:
        magic = f.read(16)

    if magic.startswith(b"MEGA65BITSTREAM0"):
        off, length, expected = parse_xilinx_bit_at(path, 4096)
        return path, "COR", off, length, expected
    if magic.startswith(b"M65J"):
        with open(path, "rb") as f:
            hdr = read_exact(f, 16)
        return path, "M65J", 16, be32(hdr[4:8]), be32(hdr[8:12])

    off, length, expected = parse_xilinx_bit_at(path, 0)
    return path, "BIT", off, length, expected


def read_response_lines(ser, cmd, timeout):
    def at_name(command):
        s = command.strip()
        u = s.upper()
        if u == "ATI":
            return "I"
        if u.startswith("ATD"):
            return "DIAL"
        if not u.startswith("AT+"):
            return None
        rest = s[3:]
        for sep in ("=", "?", " "):
            rest = rest.split(sep, 1)[0]
        return rest.upper()

    def single_line_ok(command):
        name = at_name(command)
        if name is not None:
            return name in {
                "I", "VERSION", "VER", "COREINFO", "CORE", "INFO",
                "WRITEGRANT", "AUTH", "SDMODE", "JTAGID", "JTAGSTATUS",
                "XSTATUS", "HIJACK", "MOUNT", "WIFI", "HTTP",
                "SDCARD", "SDSTATUS", "DOWNLOADSTATUS", "DLSTATUS",
                "FWSTATUS", "FIRMWARESTATUS", "THEMESTATUS", "WEBTHEMESTATUS",
                "MACHINE", "MACHINENAME", "NAME",
                "IDENTITY", "VERBOSE", "WIFIVERBOSE", "DEBUG",
            }
        return command[:1].upper() in {"V", "I", "J", "H", "M", "A", "X", "D"}

    last = time.monotonic()
    while True:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", "replace").rstrip("\r\n")
            print(text)
            last = time.monotonic()
            if text == "END" or text == "OK" or text == "NO CARRIER" or text.startswith("ERR ") or text.startswith("ERROR:") or text in {"OK P DONE", "OK S DONE"} or text.startswith("OK N DONE") or text.startswith("OK T DONE") or text.startswith("OK W DONE") or text.startswith("OK F DONE") or text.startswith("OK R DONE"):
                return not (text.startswith("ERR ") or text.startswith("ERROR"))
            if single_line_ok(cmd) and text.startswith("OK "):
                return True
        elif time.monotonic() - last > timeout:
            print(f"ERR host timeout waiting for response to {cmd}", file=sys.stderr)
            return False


def monitor_serial_lines(ser, duration=None):
    old_timeout = ser.timeout
    ser.timeout = 0.2
    deadline = time.monotonic() + duration if duration is not None else None
    print("INFO: monitoring serial responses; press Ctrl-C to stop", file=sys.stderr)
    try:
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                return
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", "replace").rstrip("\r\n")
            print(text)
    except KeyboardInterrupt:
        print("INFO: monitor stopped", file=sys.stderr)
    finally:
        ser.timeout = old_timeout


def wait_for_ready(ser, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip("\r\n")
        print(text)
        if text.startswith("OK S READY"):
            return True
        if text.startswith("ERR "):
            return False
    print("ERR host timeout waiting for OK S READY", file=sys.stderr)
    return False


def wait_for_n_ready(ser, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip("\r\n")
        print(text)
        if text.startswith("OK N READY"):
            return True
        if text.startswith("ERR "):
            return False
    print("ERR host timeout waiting for OK N READY", file=sys.stderr)
    return False


def drain_available_lines(ser):
    """Drain already-available text replies.

    Returns a terminal OK/ERR line if one was seen, otherwise None.
    This matters because final OK lines can arrive during a progress drain.
    """
    old_timeout = ser.timeout
    ser.timeout = 0
    terminal = None
    try:
        while ser.in_waiting:
            line = ser.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").rstrip("\r\n")
            print(text)
            if (
                text.startswith("ERR ")
                or text == "END"
                or text.startswith("OK S DONE")
                or text.startswith("OK N DONE")
                or text.startswith("OK T DONE")
                or text.startswith("OK P DONE")
                or text.startswith("OK W DONE")
            ):
                terminal = text
    finally:
        ser.timeout = old_timeout
    return terminal


def stream_local_file(ser, local_path, timeout):
    path, kind, payload_off, payload_len, expected = inspect_local_core(local_path)
    print(f"LOCAL {kind} payload_offset={payload_off} payload_length={payload_len} expected_idcode={expected:08x}")

    ser.reset_input_buffer()
    ser.write(f"AT+JTAGSTREAM={payload_len} {expected:08x}\n".encode("ascii"))
    ser.flush()
    if not wait_for_ready(ser, timeout=10.0):
        return 1

    sent = 0
    last_report = 0
    terminal = None
    chunk_size = 16384
    with open(path, "rb") as f:
        f.seek(payload_off)
        while sent < payload_len:
            chunk = f.read(min(chunk_size, payload_len - sent))
            if not chunk:
                print("ERR host short read from local file", file=sys.stderr)
                return 1
            ser.write(chunk)
            sent += len(chunk)
            if sent - last_report >= 262144 or sent == payload_len:
                ser.flush()
                terminal = drain_available_lines(ser) or terminal
                print(f"HOST_SENT {sent}/{payload_len}", file=sys.stderr)
                last_report = sent
    ser.flush()

    if terminal is not None:
        return 1 if terminal.startswith("ERR ") else 0

    read_response_lines(ser, "S", timeout=max(timeout, 20.0))
    return 0



def sink_local_file(ser, local_path, timeout):
    path, kind, payload_off, payload_len, expected = inspect_local_core(local_path)
    print(f"LOCAL {kind} payload_offset={payload_off} payload_length={payload_len} expected_idcode={expected:08x}")

    ser.reset_input_buffer()
    ser.write(f"AT+TESTSINK={payload_len}\n".encode("ascii"))
    ser.flush()
    if not wait_for_n_ready(ser, timeout=10.0):
        return 1

    sent = 0
    last_report = 0
    terminal = None
    chunk_size = 32768
    start = time.monotonic()
    with open(path, "rb") as f:
        f.seek(payload_off)
        while sent < payload_len:
            chunk = f.read(min(chunk_size, payload_len - sent))
            if not chunk:
                print("ERR host short read from local file", file=sys.stderr)
                return 1
            ser.write(chunk)
            sent += len(chunk)
            if sent - last_report >= 1048576 or sent == payload_len:
                ser.flush()
                terminal = drain_available_lines(ser) or terminal
                elapsed = max(time.monotonic() - start, 1e-6)
                print(f"HOST_SENT {sent}/{payload_len} {sent/elapsed/1024:.1f} KiB/s", file=sys.stderr)
                last_report = sent
    ser.flush()

    if terminal is not None:
        return 1 if terminal.startswith("ERR ") else 0

    read_response_lines(ser, "N", timeout=max(timeout, 20.0))
    return 0


def wait_for_w_ready(ser, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip("\r\n")
        print(text)
        if text.startswith("OK W READY"):
            return True
        if text.startswith("ERR "):
            return False
    print("ERR host timeout waiting for OK W READY", file=sys.stderr)
    return False


def write_local_file(ser, local_path, remote_path, timeout):
    path = os.path.expanduser(local_path)
    size = os.path.getsize(path)
    if remote_path is None:
        remote_path = os.path.basename(path)

    ser.reset_input_buffer()
    ser.write(f'AT+FILEWRITE="{remote_path}" {size}\n'.encode("utf-8"))
    ser.flush()
    if not wait_for_w_ready(ser, timeout=10.0):
        return 1

    sent = 0
    last_report = 0
    terminal = None
    chunk_size = 32768
    start = time.monotonic()
    with open(path, "rb") as f:
        while sent < size:
            chunk = f.read(min(chunk_size, size - sent))
            if not chunk:
                print("ERR host short read from local file", file=sys.stderr)
                return 1
            ser.write(chunk)
            sent += len(chunk)
            if sent - last_report >= 1048576 or sent == size:
                ser.flush()
                terminal = drain_available_lines(ser) or terminal
                elapsed = max(time.monotonic() - start, 1e-6)
                print(f"HOST_SENT {sent}/{size} {sent/elapsed/1024:.1f} KiB/s", file=sys.stderr)
                last_report = sent
    ser.flush()

    if terminal is not None:
        return 1 if terminal.startswith("ERR ") else 0

    read_response_lines(ser, "W", timeout=max(timeout, 20.0))
    return 0


def translate_manual_command(parts):
    if not parts:
        return ""

    first = parts[0]
    upper = first.upper()
    rest = " ".join(parts[1:]).strip()

    if upper.startswith("AT"):
        joined = " ".join(parts).strip()
        return "AT" + joined[2:]
    if upper == "GO64":
        return "GO64"

    if upper == "?":
        return "AT+HELP"
    if upper in {"V", "STATUS", "VERSION", "VER"}:
        return "AT+VERSION?"
    if upper in {"WIFI", "HTTP"}:
        return f"AT+{upper}?"
    if upper in {"SDCARD", "SDSTATUS"}:
        return "AT+SDCARD?"
    if upper in {"DOWNLOADSTATUS", "DLSTATUS"}:
        return "AT+DOWNLOADSTATUS?"
    if upper in {"FWSTATUS", "FIRMWARESTATUS"}:
        return "AT+FWSTATUS?"
    if upper in {"FWUPDATE", "FIRMWAREUPDATE"}:
        return "AT+FWUPDATE"
    if upper in {"THEMESTATUS", "WEBTHEMESTATUS"}:
        return "AT+THEMESTATUS?"
    if upper in {"THEMEINSTALL", "WEBTHEMEINSTALL"}:
        return "AT+THEMEINSTALL"
    if upper in {"MACHINE", "MACHINENAME", "IDENTITY"}:
        return f"AT+MACHINE={rest}" if rest else "AT+MACHINE?"
    if upper in {"IDENTIFY", "ABOUT"}:
        return "ATI"
    if upper in {"L", "LIST", "DIR"}:
        return f"AT+CORELIST={rest}" if rest else "AT+CORELIST"
    if upper in {"LS", "LL", "DETAIL", "COREDETAIL", "CORELS"}:
        return f"AT+COREDETAIL={rest}" if rest else "AT+COREDETAIL"
    if upper == "I":
        return f"AT+COREINFO={rest}"
    if upper == "T":
        return f"AT+CORETEST={rest}"
    if upper == "P":
        return f"AT+JTAGLOAD={rest}"
    if upper in {"LOAD", "PROGRAM", "JTAGLOAD"}:
        return f"AT+JTAGLOAD={rest}"
    if upper == "S":
        return f"AT+JTAGSTREAM={rest}"
    if upper == "N":
        return f"AT+TESTSINK={rest}"
    if upper == "W":
        return f"AT+FILEWRITE={rest}"
    if upper == "F":
        return f"AT+FETCH={rest}"
    if upper == "R":
        return f"AT+DOWNLOADREAD={rest}"
    if upper == "A":
        return "AT+WRITEGRANT?"
    if upper == "D":
        return f"AT+SDMODE={rest}" if rest else "AT+SDMODE?"
    if upper == "J":
        return "AT+JTAGID?"
    if upper == "X":
        return "AT+JTAGSTATUS?"
    if upper == "H":
        return f"AT+HIJACK={rest}"
    if upper == "M":
        return "AT+MOUNT"

    return " ".join(parts).strip()


def send_serial_text_command(ser, cmd, timeout, monitor=False, monitor_for=None):
    ser.reset_input_buffer()
    time.sleep(0.05)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        time.sleep(0.02)
    data = (cmd + "\n").encode("utf-8")
    try:
        written = ser.write(data)
    except Exception as e:
        print(f"ERR host serial write failed: {e}", file=sys.stderr)
        return 1
    if written != len(data):
        print(f"ERR host serial write short: {written}/{len(data)}", file=sys.stderr)
        return 1
    ok = read_response_lines(ser, cmd, timeout)
    if monitor and ok:
        monitor_serial_lines(ser, monitor_for)
    return 0 if ok else 1


def at_filename_arg(name):
    if '"' in name:
        raise SystemExit(f'filename cannot contain double quotes for AT command fallback: {name}')
    if not name or any(c.isspace() for c in name):
        return f'"{name}"'
    return name


def run_serial_command(port, command, baud, timeout, monitor=False, monitor_for=None):
    if not command:
        raise SystemExit("missing serial command")

    serial = load_serial_module()
    with serial.Serial(port, baud, timeout=timeout, write_timeout=max(timeout, 2.0)) as ser:
        # Host-to-Pico streaming command. This does NOT need an SD card on the Pico.
        if command[0].lower() in {"stream", "push", "jtag", "jtag-push", "push-local", "program-local"}:
            if len(command) != 2:
                raise SystemExit(f"{command[0]} expects exactly one .bit/.cor/.m65j filename")
            verb = command[0].lower()
            local_candidate = os.path.expanduser(command[1])
            if os.path.isfile(local_candidate):
                return stream_local_file(ser, local_candidate, timeout)
            if verb in {"push", "jtag", "jtag-push"}:
                print("NOTE local file not found; using Pico SD-card load command", file=sys.stderr)
                return send_serial_text_command(ser, f"AT+JTAGLOAD={at_filename_arg(command[1])}", timeout, monitor, monitor_for)
            raise SystemExit(f"local file not found: {command[1]}")

        if command[0].lower() in {"sink", "dummy", "rx-test"}:
            if len(command) != 2:
                raise SystemExit("sink expects exactly one local .bit/.cor/.m65j filename")
            return sink_local_file(ser, command[1], timeout)

        if command[0].lower() in {"store", "write", "upload", "install-file"}:
            if len(command) not in {2, 3}:
                raise SystemExit(f"{command[0]} expects: {command[0]} localfile [remote-name]")
            remote = command[2] if len(command) == 3 else None
            return write_local_file(ser, command[1], remote, timeout)

        if command[0].upper() in {"P", "LOAD", "PROGRAM", "JTAGLOAD"} and len(command) == 2:
            local_candidate = os.path.expanduser(command[1])
            if os.path.isfile(local_candidate):
                print("NOTE local file exists; using streaming mode, not Pico SD-card load command", file=sys.stderr)
                return stream_local_file(ser, local_candidate, timeout)

        cmd = translate_manual_command(command)
        return send_serial_text_command(ser, cmd, timeout, monitor, monitor_for)


def main(argv=None):
    global CLI_SERIAL_DEVICE, CLI_WEB_URL

    global_opts, argv = parse_leading_global_options(list(sys.argv[1:] if argv is None else argv))
    CLI_SERIAL_DEVICE = global_opts["serial"]
    CLI_WEB_URL = global_opts["url"]
    baud = global_opts["baud"] if global_opts["baud"] is not None else 2_000_000
    timeout = global_opts["timeout"] if global_opts["timeout"] is not None else 5.0
    monitor = global_opts["monitor"]
    monitor_for = global_opts["monitor_for"]

    if global_opts["list"]:
        return list_available_machines(baud)

    argv = rewrite_command_first_target(argv)
    cfg = read_client_config()

    serial_port = None
    if CLI_SERIAL_DEVICE:
        serial_port = CLI_SERIAL_DEVICE
    elif CLI_WEB_URL:
        kind, value = resolve_target_arg(CLI_WEB_URL, baud)
        if kind == "serial":
            serial_port = value
        elif kind == "web":
            CLI_WEB_URL = value
    elif argv and first_arg_is_target(argv):
        kind, value = resolve_target_arg(argv[0], baud)
        argv = argv[1:]
        if kind == "serial":
            serial_port = value
        elif kind == "web":
            CLI_WEB_URL = value
    elif configured_machine(cfg) and configured_device(cfg) is None and configured_serial_port(cfg) is None and command_can_use_default_target(argv):
        kind, value = resolve_target_arg(configured_machine(cfg), baud)
        if kind == "serial":
            serial_port = value
        elif kind == "web":
            CLI_WEB_URL = value
    elif not argv and not CLI_WEB_URL and configured_device(cfg) is None:
        serial_port = configured_serial_port(cfg) or autodetect_serial_port(baud)
        if serial_port:
            argv = ["ATI"]
    else:
        serial_port = resolve_implicit_serial_port(argv, baud, explicit_url=bool(CLI_WEB_URL))

    if serial_port:
        if not argv:
            argv = ["ATI"]
        return run_serial_command(serial_port, argv, baud, timeout, monitor, monitor_for)

    remote_result = route_remote_command(argv)
    if remote_result is not None:
        return remote_result

    ap = argparse.ArgumentParser(
        usage="%(prog)s [-h] [-s TTY] [-u URL] [-m] [--baud BAUD] [--timeout TIMEOUT] [target-or-command] ...",
        description="MEGA65 Expansion Board Integrated JTAG firmware utility client (experimental v0.1)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent(
            """\
            Common commands:
              m65j.py -l
              m65j.py mymega list
              m65j.py r6:mymega push core.bit --board 6
              m65j.py -s /dev/ttyACM0 ATI
              m65j.py -m AT+FETCHNOW=6
              m65j.py --monitor-for 30 AT+WIFI?
              m65j.py -s /dev/ttyACM0 push local.bit
              m65j.py -u http://mega65-jtag.local status
              m65j.py -u http://mega65-jtag.local push core.bit --board 6
              m65j.py wifi
              m65j.py sdcard
              m65j.py status
              m65j.py keys
              m65j.py check sdcard/cores
              m65j.py bless --board 6 core.bit
              m65j.py mirror stable sdcard/cores --board all --overwrite --bless --yes
              m65j.py populate stable --board all --overwrite --yes
              m65j.py list [--board 6]
              m65j.py push [url] core.bit --board 6
              m65j.py store [url] core.bit --board 6
              m65j.py load [url] /cores/core.cor --board 6
              m65j.py get [url] /cores/core.cor -o core.cor
              m65j.py downloadstatus
              m65j.py downloads-get [url] download-00.dat -o fetched.dat
              m65j.py put http://host/files/core.bit core.bit --board 6
              m65j.py /dev/ttyACM0 ATI
              m65j.py /dev/ttyACM0 stream local.bit

            Global target options may be placed before the command:
              -l, --list-machines   list USB and local /24 HTTP machines
              -m, --monitor         keep serial open after command and print later lines
              -s, --device <tty>     serial TTY, e.g. /dev/ttyACM0
              -u, --url <url|name>   board HTTP URL, IP address, or machine name

            The client reads .m65j.config, then ~/.m65j.config.
            Use `serial=/dev/ttyACM0` for the USB TTY and `url=http://<pico-ip>`
            or `ip=<pico-ip>` for the HTTP URL. Use `machine=mymega` to resolve
            by machine name, checking USB first and then local /24 HTTP. If no
            target is configured, a single connected MEGA65 JTAG Pico USB CDC
            device is auto-detected.
            """
        ),
    )
    ap.add_argument("port", metavar="target-or-command",
                    help="legacy serial port form, e.g. /dev/ttyACM0 ATI")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="serial command to send, e.g. ATI, AT+JTAGID?, AT+JTAGLOAD=/core.bit, or stream local.bit")
    ap.add_argument("-s", "--device", "--serial", dest="serial_device", help="serial TTY, e.g. /dev/ttyACM0")
    ap.add_argument("-u", "--url", dest="web_url", help="board HTTP URL, IP address, or machine name")
    ap.add_argument("-l", "--list-machines", action="store_true", help="list USB and local /24 HTTP machines")
    ap.add_argument("-m", "--monitor", "--follow", action="store_true",
                    help="keep serial open after command and print later unsolicited responses")
    ap.add_argument("--monitor-for", type=float, default=None,
                    help="monitor serial responses for this many seconds after the command")
    ap.add_argument("--baud", type=int, default=2_000_000)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args(argv)
    if args.list_machines:
        return list_available_machines(args.baud)
    if not args.command:
        ap.error("missing command")
    return run_serial_command(args.serial_device or args.port,
                              args.command,
                              args.baud,
                              args.timeout,
                              args.monitor or args.monitor_for is not None,
                              args.monitor_for)


if __name__ == "__main__":
    raise SystemExit(main())
