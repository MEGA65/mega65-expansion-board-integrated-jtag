#!/usr/bin/env python3
import argparse
import importlib.util
import os
import sys
import time
from pathlib import Path


def load_serial_module():
    try:
        import serial
    except ImportError:
        print("Install pyserial for serial-port commands: python3 -m pip install pyserial", file=sys.stderr)
        raise
    return serial


def load_bless_core_module():
    path = Path(__file__).with_name("bless-core.py")
    spec = importlib.util.spec_from_file_location("m65_bless_core", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load signing helper: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_bless_core(argv):
    return load_bless_core_module().main(argv)


def route_remote_command(argv):
    if not argv:
        return None

    verb = argv[0].lower()
    rest = argv[1:]

    if verb in {"keys", "--keys"}:
        return run_bless_core(["--keys", *rest])

    if verb in {"bless", "sign"}:
        if "--bless" not in rest:
            rest = ["--bless", *rest]
        return run_bless_core(rest)

    if verb in {"push", "jtag", "jtag-push"}:
        if "--device" in rest or "--put" in rest:
            return run_bless_core(rest)
        if len(rest) < 2:
            raise SystemExit("push expects: push <device-url> <core.bit|core.cor> [bless options]")
        device, input_path, *extra = rest
        return run_bless_core(["--device", device, *extra, input_path])

    if verb in {"store", "store-file", "http-store"}:
        if "--device" in rest:
            return run_bless_core(["--store-only", *rest])
        if len(rest) < 2:
            raise SystemExit("store expects: store <device-url> <core.bit|core.cor> [bless options]")
        device, input_path, *extra = rest
        return run_bless_core(["--device", device, "--store-only", *extra, input_path])

    if verb in {"put", "http-put"}:
        if "--put" in rest:
            return run_bless_core(rest)
        if len(rest) < 2:
            raise SystemExit("put expects: put <exact-put-url> <core.bit|core.cor> [bless options]")
        url, input_path, *extra = rest
        return run_bless_core(["--put", url, *extra, input_path])

    if verb in {"signing", "bless-core"}:
        return run_bless_core(rest)

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
            "put <exact-put-url> file [options]. Serial commands keep the existing form: "
            "uart_client.py <port> ATI or uart_client.py <port> stream local.bit."
        ),
    )
    ap.add_argument("port")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="Command to send, e.g. V, J, P /core.bit, or stream local.bit")
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
