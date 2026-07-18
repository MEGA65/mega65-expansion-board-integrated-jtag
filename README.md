# MEGA65 Expansion Board Integrated JTAG Firmware & Utilities

Experimental Pi Pico / RP2040 firmware and host utilities for SD-backed,
UART/USB, and WiFi-assisted JTAG loading of MEGA65-style Xilinx 7-series
bitstreams.

**Status:** v0.1 experimental. The board wiring, pin assignments, AT/BASIC
command protocol, HTTP endpoints, SD-card config files, signed-file container,
and host utility command line are all still subject to change.

Current major pieces:

- 2 Mbps 8N1 command UART on GPIO pins
- native Pico USB CDC command interface, no extra USB-UART adapter required
- SPI microSD + FatFs by default
- direct `.bit`, simple MEGA65 `.cor`, and simple `.m65j` input support
- single-device JTAG chain assumption
- GPIO-controlled JTAG hijack switch
- CPU bit-banged JTAG first; PIO can come later after bring-up

## Default Pins

These are the current firmware defaults, not a stable hardware interface.
Check `include/config.h` and `PINOUT.md` before building hardware or wiring a
new board.

Edit `include/config.h` for your board. See also `PINOUT.md` for the TE0790/XMOD socket mapping.

```text
GP0   UART TX, normally to TE0790/XMOD J2 B
GP1   UART RX, normally to TE0790/XMOD J2 A

GP2   SD CS
GP3   SD MOSI
GP4   SD SCK
GP5   SD MISO

GP6   JTAG TCK, normally to TE0790/XMOD J2 C through hijack switch
GP7   JTAG TMS, normally to TE0790/XMOD J2 H through hijack switch
GP8   JTAG TDI, normally to TE0790/XMOD J2 F through hijack switch
GP9   JTAG TDO, normally to TE0790/XMOD J2 D; listen-only/shared is usually OK

GP10  JTAG_HIJACK, active high by default
GP11  WRITE_ENABLE, active-low physical write-authority button/jumper
GP12  spare
```

A 4-bit bus switch can arbitrate TCK/TMS/TDI/TDO cleanly. The switch should default to TE0790 passthrough when `JTAG_HIJACK` is inactive.

The default SD transport is `AUTO`: the firmware probes the first fabbed
schematic pinout with bit-banged SPI, then the corrected hardware-SPI pinout.
Use `AT+SDMODE=auto`, `AT+SDMODE=soft`, or `AT+SDMODE=hw` before mounting SD to
force a mode at runtime.

## Build

On Ubuntu/Debian:

```bash
make deps
make
```

The default build is now Pico W, WiFi remote support enabled, and FatFs-enabled.
It will clone:

- Raspberry Pi `pico-sdk` into `.deps/pico-sdk`
- `carlk3/no-OS-FatFS-SD-SPI-RPi-Pico` into `third_party/no-OS-FatFS-SD-SPI-RPi-Pico`

This project adds the FatFs backend
from the repository's `FatFs_SPI/` subdirectory. The repo root itself does not
have a `CMakeLists.txt`.

Output:

```text
build-wifi/mega65-pico-jtag.uf2
```

For a non-WiFi Pico build:

```bash
make PICO_BOARD=pico ENABLE_WIFI_REMOTE=0 BUILD_DIR=build build
```

For a UART/JTAG-only bring-up build without SD/FatFs:

```bash
make nofatfs
```

or:

```bash
make USE_FATFS=0 build
```

## Flashing

The Makefile has both `upload` and `flash`; they are aliases.

```bash
make flash
```

It tries, in order:

1. system `picotool`
2. local `.deps/picotool/build/picotool`, if you have built it with `make picotool`
3. UF2 mass-storage copy to an `RPI-RP2` or `RP2350` BOOTSEL volume

The simplest method is still UF2:

```bash
# Hold BOOTSEL while plugging in the Pico, then:
make upload-uf2
```

If autodetection fails:

```bash
make upload-uf2 PICO_MOUNT=/media/$USER/RPI-RP2
```

## Command ports

The same line-oriented ASCII protocol is accepted on both:

- GP0/GP1 hardware UART, 2 Mbps 8N1
- native Pico USB CDC serial, usually `/dev/ttyACM0` on Linux

Replies are currently broadcast to both ports. This is intentional and keeps MEGA65-side and PC-side control simple.

## UART Commands

The command set is AT-style by default and remains experimental. Command names,
responses, fields, and error text may change while the firmware and utilities
are still at v0.1.

```text
AT                         modem attention check
ATI                        identify firmware and WiFi capability
ATD*                       novelty dial command
AT+GO64 or GO64            enter BASIC command mode
AT+VERSION?                firmware version and transport status
AT+CORELIST[=path]         list .BIT/.COR/.M65J files and dirs
AT+COREDETAIL[=path]       detailed list with COR title/version/board
AT+COREINFO=file           inspect core file
AT+CORETEST=file           SD read-speed test; read payload and discard
AT+JTAGLOAD=file           hijack JTAG and program existing SD core
AT+JTAGSTREAM=len idcode   stream raw payload over serial; USB-only by default
AT+TESTSINK=len            serial receive/discard speed test
AT+FILEWRITE=file len      write a core file to SD; USB + physical WE required by default
AT+FETCH=url name          fetch http:// URL into DOWNLOADS/name
AT+DOWNLOADREAD=name       read DOWNLOADS/name as raw bytes
AT+AUTOFETCH[=0|1]         show/set mirror auto-update enable
AT+FETCHSTATUS?            show mirror auto-update/fetch status
AT+FETCHNOW[=3|6|remote]   fetch mirror manifest now
AT+FETCHINTERVAL[=hours]   show/set auto-update interval; min 3
AT+FETCHBOARD=3|6|remote   select autofetch board manifest
AT+VERBOSE[=0|1|2]         show/set WiFi/fetch diagnostic verbosity
AT+MACHINE[=name]          show/set machine name; saved in AT_SETTINGS.cfg
ATS60?                     seconds since last successful auto-fetch
ATS61?                     auto-fetch running flag
AT&W                       save AT settings to SD card
ATZ                        soft reboot Pico and reload saved settings
AT+WRITEGRANT?             show physical write-authority status
AT+REMOTE?                 show parsed mega65-jtag.cfg
AT+WIFI?                   show live WiFi/HTTP status
AT+WIFIPROBE               retry CYW43 hardware probe now
AT+SDCARD?                 show SD card media/mount status
AT+SDMODE[=auto|hw|soft]   show/set SD transport before mount
AT+JTAGID?                 read JTAG IDCODE, using hijack
AT+JTAGSTATUS?             read Xilinx BOOTSTS/STAT/BYPASS via CFG_OUT
AT+HIJACK=1|0              manually assert/release JTAG hijack
AT+MOUNT                   mount/remount SD card
AT+HELP                    help
```

While this firmware is running, pressing the Pico's onboard BOOTSEL button
reboots it into USB BOOTSEL/UF2 mode when the firmware's periodic poll catches
it. The external GP11 switch remains the physical write-grant input only.

Example:

```bash
python3 tools/m65j.py /dev/ttyACM0 ATI
# or via the hardware UART:
python3 tools/m65j.py /dev/ttyUSB0 AT+VERSION?
python3 tools/m65j.py /dev/ttyACM0 AT+CORELIST=/
python3 tools/m65j.py /dev/ttyACM0 AT+COREDETAIL=/
python3 tools/m65j.py /dev/ttyACM0 AT+COREINFO=/cores/mega65r6.cor
python3 tools/m65j.py /dev/ttyACM0 AT+JTAGLOAD=/cores/mega65r6.cor
```

Set `M65_ENABLE_LEGACY_UART_COMMANDS=1` at compile time only if you need the
early bring-up one-letter protocol. `tools/m65j.py` still translates the
old short forms for convenience.

## File handling

`.bit` files are parsed by skipping the Xilinx wrapper and streaming the raw
configuration payload.

`.cor` support is currently the simple MEGA65 bit2core layout assumption:

```text
0x0000  "MEGA65BITSTREAM0"
0x0010  core title, NUL-padded
0x0030  core version, NUL-padded
0x0050  target model string, NUL-padded
0x0070  target model ID / board revision byte
0x1000  embedded original .bit file
```

`.m65j` is a small raw wrapper useful for later tools:

```text
+0   "M65J"
+4   payload length, big endian
+8   expected IDCODE, big endian; zero disables check
+12  reserved
+16  raw Xilinx configuration payload
```

## Notes

The firmware assumes exactly one JTAG device. That matches the target board and
keeps the loader small: no bypass padding, no chain scan, no table of devices.

The JTAG bit ordering is:

- IR instructions: LSB-first
- IDCODE/status DR reads: LSB-first
- Xilinx CFG_IN payload bytes: MSB-first per byte

## Xilinx status / close-out diagnostics

This build uses conservative post-configuration close-out clocks and adds Xilinx
configuration-register diagnostics via JTAG `CFG_IN`/`CFG_OUT`.

Command:

```text
AT+JTAGSTATUS?             read JTAG IDCODE plus Xilinx BOOTSTS/STAT/BYPASS
```

After `AT+JTAGLOAD=file` or `stream <local.bit>`, the firmware prints a
diagnostic line before the terminal `OK ... DONE` line:

```text
XSTAT P idcode=13636093 bootsts=xxxxxxxx stat=xxxxxxxx bypass=xxxxxxxx done=1 release_done=1 eos=1 startup=...
OK P DONE
```

or for streamed host payloads:

```text
XSTAT S ...
OK S DONE
```

The `done`, `release_done`, `eos`, and `startup` fields are decoded from the
raw STAT word using the same fields that the original `mega65-tools` JTAG path
logged.  The raw `bootsts` and `stat` values are the important data for comparing
a working MEGA65 core with a failing AExp/Amiga core.

Caveat: the CFG_OUT readback path is still a first-cut single-device-chain
implementation.  The raw values are diagnostic; if they look byte/bit-swapped,
compare working-vs-failing runs rather than trusting the decoded fields yet.


## Write-gate policy

Normal operation treats the SD card as read-only: the loader opens existing core
files for reading and can load them, but it does not write to the filesystem
unless a guarded write command is accepted.

The guarded file-write command is:

```text
AT+FILEWRITE=<file.bit|file.cor|file.m65j> <length>
```

Current defaults are deliberately conservative:

```text
M65_WRITE_ENABLE_PIN         GP11
M65_WRITE_ENABLE_ACTIVE_LOW  1      # button/jumper to GND
M65_WRITE_ENABLE_TIMEOUT_MS  120000 # 2 minute authority window
M65_WRITE_COMMANDS_USB_ONLY  1      # MEGA65-side UART cannot write files
M65_STREAM_COMMANDS_USB_ONLY 1      # MEGA65-side UART cannot stream arbitrary bitstreams
```

So the MEGA65-accessible UART can list and launch already-installed cores, but
cannot persistently install new FPGA code. Users can either remove the SD card
and copy files on a PC, or connect USB, assert the physical write-enable input,
and upload with:

```bash
python3 tools/m65j.py /dev/ttyACM0 write local-core.bit remote-core.bit
```

The upload is written to `remote-core.bit.partial`, synced, then renamed into place
after a complete transfer. Raw sector writes are intentionally not part of the
current protocol.

## Signed remote files and downloads

Remote HTTP uploads and firmware-side URL fetches can require a signed trailer.
The format is documented in [SIGNED_CORE_FORMAT.md](SIGNED_CORE_FORMAT.md).

Use `tools/m65j.py keys` to list local public keys and print the
`trusted_key=` lines to copy into `mega65-jtag.cfg`. The same client can
bless, store, or JTAG-push a core over HTTP:

```sh
tools/m65j.py check sdcard/cores
tools/m65j.py bless --board 6 core.bit
tools/m65j.py -u http://mega65-jtag.local push core.bit --board 6
tools/m65j.py -u http://mega65-jtag.local store core.bit --board 6
tools/m65j.py -s /dev/ttyACM0 push local.bit
```

`tools/m65j.py` reads `.m65j.config` from the current directory first, then
`~/.m65j.config`. Use `serial=` for the USB TTY, `url=` for the HTTP
interface, or `machine=` for a named board:

```ini
serial=/dev/ttyACM0
url=http://mega65-jtag.local
# or:
ip=192.168.1.65
# or:
machine=mymega
```

With that in place, the target can be omitted. Raw AT-style and one-letter
commands use the configured serial port; web commands use the configured URL:

```sh
tools/m65j.py -l
tools/m65j.py status
tools/m65j.py mymega list
tools/m65j.py r6:mymega push core.bit --board 6
tools/m65j.py list --board 6
tools/m65j.py check sdcard/cores
tools/m65j.py push core.bit --board 6
tools/m65j.py load /cores/mega65r6.cor --board 6
tools/m65j.py get /cores/mega65r6.cor -o mega65r6.cor
tools/m65j.py downloads-get fetched.bit -o fetched.bit
tools/m65j.py mirror stable sdcard/cores --board all --overwrite --bless --yes
tools/m65j.py populate stable --board all --overwrite --yes
```

`AT+MACHINE=name` accepts 1 to 24 ASCII letters, digits, dot, dash, or
underscore. The value is saved to `AT_SETTINGS.cfg`, appears in `ATI` and
`AT+VERSION?`, and is appended to the USB product string after USB
re-enumerates. On boot, settings are loaded after the normal USB-safe delay and
the firmware briefly reconnects USB if a saved machine name is present.

If neither `serial=` nor `url=`/`ip=`/`machine=` is configured and no
`-s`/`-u` option or machine-name target is given, `m65j.py` auto-detects a
single connected MEGA65 JTAG Pico USB CDC device. New firmware identifies
itself to the host as product `MEGA65 Expansion Board Integrated JTAG`
optionally followed by the machine name; older builds are recognised by a brief
`ATI` probe on Raspberry Pi Pico CDC ports. Machine-name targets check matching
USB devices first, then scan port 80 on each local `/24` subnet for
`GET /identity`. Use `m65j.py -l` to list discovered USB and HTTP machines.

The HTTP identity endpoint is intentionally tiny and unauthenticated for
discovery:

```text
GET /identity    returns r3:name, r6:name, or r0:name
```

`mirror` runs on the host and uses the alt-core/filehost downloader. Supply a
release tag such as `stable`, `unstable`, or `nightly` as the first positional
argument; if the tag is omitted, `stable` is used. It canonicalises core
filenames by stripping version/date/build tokens, and writes a matching
`<tag>-r3.sha256` and/or `<tag>-r6.sha256` manifest. Each non-comment manifest line is
`<sha256>  <relative-filename>`, with hashes over the unsigned payload bytes so
re-signing does not create false updates. `mirror --bless` signs both the core
files and every manifest. `populate` does the same staging work and then PUTs the
manifest-listed files and signed manifests to the board SD card over HTTP.

`check` prints `[blessed]`, `[uncursed]`, or `[cursed]` for local files. A
blessed file has a valid trailer according to the local keys, `--trusted-key`,
or `--remote-config mega65-jtag.cfg`; uncursed means no trailer, and cursed
means a bad or untrusted trailer.

If files.mega65.org requires a logged-in browser session, place the raw Cookie
header in `.m65j.config` or `~/.m65j.config`, or cache login credentials there:

```ini
filehost_cookie=PHPSESSID=...; other_cookie=...
# or:
filehost_user=you@example.com
filehost_password=secret
```

Firmware-side fetch support remains deliberately small:
`AT+FETCH=<http://url> <name>` fetches one explicit HTTP URL into
`DOWNLOADS/<name>`. For unattended updates, set `fetch_base_url`,
`fetch_channel`, `fetch_board=3|6`, and `autofetch=1` in `mega65-jtag.cfg`.
For channel `stable` and board `6`, the firmware fetches
`stable-r6.sha256`, verifies the signed manifest first, then fetches only
changed signed core files listed in it. `AT+FETCHSTATUS?` reports status
without interrupting a running fetch, `AT+FETCHNOW[=3|6|remote]` starts a
mirror fetch immediately, `AT+AUTOFETCH=0|1` overrides enablement,
`AT+FETCHINTERVAL=<hours>` sets the interval with a minimum of 3 hours,
`AT+FETCHBOARD=3|6|remote` selects the board-specific manifest, and
`AT&W`/`ATZ` save/reload those AT settings from `AT_SETTINGS.cfg`. `ATS60?`
reports seconds since the last successful auto-fetch and `ATS61?` reports the
running flag. Changing autofetch settings or saving AT settings requires the
physical write grant; triggering `AT+FETCHNOW` or scheduled autofetch from an
already configured, trusted mirror does not.

`AT+VERBOSE=0|1|2` controls unsolicited WiFi/fetch diagnostics. Level 0 is
quiet, level 1 reports WiFi state changes and fetch start/finish/failure, and
level 2 also reports fetch progress and manifest scan checks. `AT&W` saves the
selected level.

While an autofetch is running, browser requests return a cached busy page
instead of interrupting the transfer. The page is loaded from
`WWW/fetch_busy.html` during HTTP startup and can use `{FETCH_*}` substitutions
for the current file, manifest count, byte count, rate, and status. The stop
button targets `GET /fetch/stop`, which explicitly cancels the fetch. The
firmware also caches `WWW/favicon-32x32.png` and serves it from `/favicon.ico`
without touching the SD card during a fetch.

Useful commands:

```text
AT+FETCH=<http://url> <name>       fetch to DOWNLOADS/<name>
AT+DOWNLOADREAD=<name>             read DOWNLOADS/<name>
AT+FETCHSTATUS?                    show mirror fetch status
AT+FETCHNOW[=3|6|remote]           start mirror fetch immediately
AT+FETCHBOARD=3|6|remote           select autofetch board manifest
AT+MACHINE[=name]                  show/set discovery machine name
```

`mega65-jtag.cfg` controls enforcement:

```ini
require_signatures=1
trusted_key=p256:04...
```

When signatures are required, unsigned or badly signed uploads/fetches are
deleted instead of being committed. `PUT /jtag` spools signed transfers to SD,
verifies them, and only then programs JTAG.
