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
    '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'
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
