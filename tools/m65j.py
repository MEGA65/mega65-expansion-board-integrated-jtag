#!/usr/bin/env python3
import argparse
import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time
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
CONFIG_FILES = (Path(".m65j.config"), Path.home() / ".m65j.config")


def load_serial_module():
    try:
        import serial
    except ImportError:
        print("Install pyserial for serial-port commands: python3 -m pip install pyserial", file=sys.stderr)
        raise
    return serial


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
    print("\nTo trust a key, copy its trusted_key= line into REMOTE_ENABLE.cfg on the SD card.")
    print("For enforcement, also set: require_signatures=1")
    return 0


def file_type_for(path):
    ext = path.suffix.lower()
    if ext == ".bit":
        return 1
    if ext == ".cor":
        return 2
    if ext == ".m65j":
        return 3
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


def configured_device(cfg=None):
    cfg = read_client_config() if cfg is None else cfg
    device = cfg.get("device") or cfg.get("url") or cfg.get("base_url")
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
        "No web device configured. Add device=http://<pico-ip> or ip=<pico-ip> "
        "to .m65j.config or ~/.m65j.config, or pass the device URL."
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
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read(), r.headers.get_content_type()


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
        user, password = config_auth(args.user, args.password)
        put_file(put_url, signed, user, password)
    elif args.bless and args.device and not wrote:
        raise SystemExit("--bless requested but no output was written")
    return 0


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


def pop_optional_device(args):
    if first_arg_is_device(args):
        return args[0], args[1:]
    return None, args


def route_web_command(verb, rest):
    if verb in {"status", "index", "home"}:
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

    routed = route_web_command(verb, rest)
    if routed is not None:
        return routed

    if verb.startswith("at") or verb == "go64":
        return route_at_over_web(argv)

    if verb in {"keys", "--keys"}:
        return signing_main(["--keys", *rest])

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
                "XSTATUS", "HIJACK", "MOUNT",
            }
        return command[:1].upper() in {"V", "I", "J", "H", "M", "A", "X", "D"}

    last = time.monotonic()
    while True:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", "replace").rstrip("\r\n")
            print(text)
            last = time.monotonic()
            if text == "END" or text == "OK" or text == "NO CARRIER" or text.startswith("ERR ") or text in {"OK P DONE", "OK S DONE"} or text.startswith("OK N DONE") or text.startswith("OK T DONE") or text.startswith("OK W DONE") or text.startswith("OK F DONE") or text.startswith("OK R DONE"):
                break
            if single_line_ok(cmd) and text.startswith("OK "):
                break
        elif time.monotonic() - last > timeout:
            break


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

    if upper.startswith("AT") or upper == "GO64":
        return " ".join(parts).strip()

    if upper == "?":
        return "AT+HELP"
    if upper == "V":
        return "AT+VERSION?"
    if upper == "L":
        return f"AT+CORELIST={rest}" if rest else "AT+CORELIST"
    if upper == "I":
        return f"AT+COREINFO={rest}"
    if upper == "T":
        return f"AT+CORETEST={rest}"
    if upper == "P":
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


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    remote_result = route_remote_command(argv)
    if remote_result is not None:
        return remote_result

    ap = argparse.ArgumentParser(
        description="Single client for pico-m65jtag serial, signing, and HTTP/JTAG delivery",
        epilog=(
            "Signing/HTTP commands: keys; bless [options] file; "
            "push <device-url> file [options]; store <device-url> file [options]; "
            "put <exact-put-url> file [options]. Web commands can use .m65j.config: "
            "status; load file; get file; downloads-get name. Serial commands keep the form: "
            "m65j.py <port> ATI or m65j.py <port> stream local.bit."
        ),
    )
    ap.add_argument("port", help="serial port, e.g. /dev/ttyACM0 or COM3")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="serial command to send, e.g. ATI, AT+JTAGID?, AT+JTAGLOAD=/core.bit, or stream local.bit")
    ap.add_argument("--baud", type=int, default=2_000_000)
    ap.add_argument("--timeout", type=float, default=1.0)
    args = ap.parse_args(argv)
    if not args.command:
        ap.error("missing command")

    serial = load_serial_module()
    with serial.Serial(args.port, args.baud, timeout=args.timeout, write_timeout=None) as ser:
        # Host-to-Pico streaming command. This does NOT need an SD card on the Pico.
        if args.command[0].lower() in {"stream", "push-local", "program-local"}:
            if len(args.command) != 2:
                ap.error("stream expects exactly one local .bit/.cor/.m65j filename")
            sys.exit(stream_local_file(ser, args.command[1], args.timeout))

        if args.command[0].lower() in {"sink", "dummy", "rx-test"}:
            if len(args.command) != 2:
                ap.error("sink expects exactly one local .bit/.cor/.m65j filename")
            sys.exit(sink_local_file(ser, args.command[1], args.timeout))

        if args.command[0].lower() in {"write", "upload", "install-file"}:
            if len(args.command) not in {2, 3}:
                ap.error("write expects: write localfile [remote-name]")
            remote = args.command[2] if len(args.command) == 3 else None
            sys.exit(write_local_file(ser, args.command[1], remote, args.timeout))

        if args.command[0].upper() == "P" and len(args.command) == 2:
            local_candidate = os.path.expanduser(args.command[1])
            if os.path.isfile(local_candidate):
                print("NOTE local file exists; using streaming mode, not Pico SD-card P command", file=sys.stderr)
                sys.exit(stream_local_file(ser, local_candidate, args.timeout))

        cmd = translate_manual_command(args.command)
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        read_response_lines(ser, cmd, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
