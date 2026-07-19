#!/usr/bin/env python3
import argparse
import base64
import concurrent.futures
import hashlib
import ipaddress
import os
import re
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import socket
import urllib.error
import urllib.parse
import urllib.request
import zlib
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

CLI_SERIAL_DEVICE = None
CLI_WEB_URL = None


# m65j.py is intentionally self-contained.  The mirror/populate implementation
# is embedded here so users can copy this one file without also copying helper
# modules.
_EMBEDDED_DOWNLOAD_ALTCORES_B64 = (
    "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"
)
_ALTCORE_NS = None


def embedded_altcore_bless_core(path, board, args):
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


def embedded_altcore_main(argv):
    global _ALTCORE_NS
    if _ALTCORE_NS is None:
        import types

        source = zlib.decompress(base64.b64decode(_EMBEDDED_DOWNLOAD_ALTCORES_B64)).decode("utf-8")
        module_name = "m65j_embedded_download_altcores"
        module = types.ModuleType(module_name)
        module.__file__ = __file__
        sys.modules[module_name] = module
        ns = module.__dict__
        exec(compile(source, "<m65j-embedded-download-altcores>", "exec"), ns)
        ns["bless_core"] = embedded_altcore_bless_core
        _ALTCORE_NS = ns
    return _ALTCORE_NS["main"](argv)


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


def latest_main(argv):
    return embedded_altcore_main(argv)


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
    ap.add_argument("--source-url", dest="option_source_url", action="append", default=[],
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


def downloader_args_from(ns, output, release_type, source_urls, bless):
    args = [
        "--board", normalize_board_for_mirror(ns.board),
        "--output", str(output),
        "--cache", ns.cache,
        "--channel", release_type,
    ]
    for attr in (
        "cookie", "cookie_file", "manifest", "key", "key_name", "hash_file", "detail_workers",
        "firmware", "firmware_version", "firmware_build", "theme_name", "theme_version",
    ):
        value = getattr(ns, attr, None)
        if value is not None and value != "":
            args.extend([f"--{attr.replace('_', '-')}", str(value)])
    for url in [*getattr(ns, "option_source_url", []), *source_urls]:
        args.extend(["--source-url", url])
    for extra in getattr(ns, "extra_core", []) or []:
        args.extend(["--extra-core", str(extra)])
    for theme in getattr(ns, "theme", []) or []:
        args.extend(["--theme", str(theme)])
    if ns.keep_zips:
        args.append("--keep-zips")
    if ns.overwrite:
        args.append("--overwrite")
    if ns.limit:
        args.extend(["--limit", str(ns.limit)])
    if bless:
        args.append("--bless")
    if getattr(ns, "yes", False):
        args.append("--yes")
    if getattr(ns, "blank_filename", False):
        args.append("--blank-filename")
    if getattr(ns, "no_hash_file", False):
        args.append("--no-hash-file")
    if getattr(ns, "preserve_filenames", False):
        args.append("--preserve-filenames")
    if getattr(ns, "dry_run", False):
        args.append("--dry-run")
    if getattr(ns, "quiet", False):
        args.append("--quiet")
    return args


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
    args = downloader_args_from(ns, output, release_type, source_urls, ns.bless)
    return embedded_altcore_main(args)


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
        args = downloader_args_from(ns, out_dir, ns.release_type, ns.source_urls, bless)
        rc = embedded_altcore_main(args)
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
        "CORELS", "I", "T", "P", "S", "N", "W", "F", "R", "A", "D",
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
