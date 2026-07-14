# pico_m65jtag

Minimal Pi Pico / RP2040 JTAG core loader for MEGA65-style Xilinx 7-series bitstreams.

This is the non-FPGA version of the tiny loader:

- 2 Mbps 8N1 command UART on GPIO pins
- native Pico USB CDC command interface, no extra USB-UART adapter required
- SPI microSD + FatFs by default
- direct `.bit`, simple MEGA65 `.cor`, and simple `.m65j` input support
- single-device JTAG chain assumption
- GPIO-controlled JTAG hijack switch
- CPU bit-banged JTAG first; PIO can come later after bring-up

## Default pins

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
Use `D auto`, `D soft`, or `D hw` before mounting SD to force a mode at runtime.

## Build

On Ubuntu/Debian:

```bash
make deps
make
```

The default build is now FatFs-enabled and will clone:

- Raspberry Pi `pico-sdk` into `.deps/pico-sdk`
- `carlk3/no-OS-FatFS-SD-SPI-RPi-Pico` into `third_party/no-OS-FatFS-SD-SPI-RPi-Pico`

This project adds the FatFs backend
from the repository's `FatFs_SPI/` subdirectory. The repo root itself does not
have a `CMakeLists.txt`.

Output:

```text
build/pico_m65jtag.uf2
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

## UART commands

```text
AT                         modem attention check
ATI                        identify firmware and WiFi capability
ATD*                       novelty dial command
AT+GO64 or GO64            enter BASIC command mode
AT+VERSION?                firmware version and transport status
AT+CORELIST[=path]         list .BIT/.COR/.M65J files and dirs
AT+COREINFO=file           inspect core file
AT+CORETEST=file           SD read-speed test; read payload and discard
AT+JTAGLOAD=file           hijack JTAG and program existing SD core
AT+JTAGSTREAM=len idcode   stream raw payload over serial; USB-only by default
AT+TESTSINK=len            serial receive/discard speed test
AT+FILEWRITE=file len      write a core file to SD; USB + physical WE required by default
AT+FETCH=url name          fetch http:// URL into DOWNLOADS/name
AT+DOWNLOADREAD=name       read DOWNLOADS/name as raw bytes
AT+WRITEGRANT?             show physical write-authority status
AT+REMOTE?                 show parsed REMOTE_ENABLE.cfg
AT+SDMODE[=auto|hw|soft]   show/set SD transport before mount
AT+JTAGID?                 read JTAG IDCODE, using hijack
AT+JTAGSTATUS?             read Xilinx BOOTSTS/STAT/BYPASS via CFG_OUT
AT+HIJACK=1|0              manually assert/release JTAG hijack
AT+MOUNT                   mount/remount SD card
AT+HELP                    help
```

Example:

```bash
python3 tools/uart_client.py /dev/ttyACM0 ATI
# or via the hardware UART:
python3 tools/uart_client.py /dev/ttyUSB0 AT+VERSION?
python3 tools/uart_client.py /dev/ttyACM0 AT+CORELIST=/
python3 tools/uart_client.py /dev/ttyACM0 AT+COREINFO=/cores/mega65r6.cor
python3 tools/uart_client.py /dev/ttyACM0 AT+JTAGLOAD=/cores/mega65r6.cor
```

Set `M65_ENABLE_LEGACY_UART_COMMANDS=1` at compile time only if you need the
early bring-up one-letter protocol. `tools/uart_client.py` still translates the
old short forms for convenience.

## File handling

`.bit` files are parsed by skipping the Xilinx wrapper and streaming the raw
configuration payload.

`.cor` support is currently the simple MEGA65 bit2core layout assumption:

```text
0x0000  "MEGA65BITSTREAM0"
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

Release defaults are deliberately conservative:

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
python3 tools/uart_client.py /dev/ttyACM0 write local-core.bit remote-core.bit
```

The upload is written to `remote-core.bit.tmp`, synced, then renamed into place
after a complete transfer. Raw sector writes are intentionally not part of the
release protocol.

## Signed remote files and downloads

Remote HTTP uploads and firmware-side URL fetches can require a signed trailer.
The format is documented in [SIGNED_CORE_FORMAT.md](SIGNED_CORE_FORMAT.md).

Use `tools/uart_client.py keys` to list local public keys and print the
`trusted_key=` lines to copy into `REMOTE_ENABLE.cfg`. The same client can
bless, store, or JTAG-push a core over HTTP:

```sh
tools/uart_client.py bless --board 6 core.bit
tools/uart_client.py push http://mega65-jtag.local core.bit --board 6
tools/uart_client.py store http://mega65-jtag.local core.bit --board 6
```

Useful commands:

```text
AT+FETCH=<http://url> <name>       fetch to DOWNLOADS/<name>
AT+DOWNLOADREAD=<name>             read DOWNLOADS/<name>
```

`REMOTE_ENABLE.cfg` controls enforcement:

```ini
require_signatures=1
trusted_key=p256:04...
```

When signatures are required, unsigned or badly signed uploads/fetches are
deleted instead of being committed. `PUT /jtag` spools signed transfers to SD,
verifies them, and only then programs JTAG.
