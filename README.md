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

GP2   SD SCK
GP3   SD MOSI
GP4   SD MISO
GP5   SD CS

GP6   JTAG TCK, normally to TE0790/XMOD J2 C through hijack switch
GP7   JTAG TMS, normally to TE0790/XMOD J2 H through hijack switch
GP8   JTAG TDI, normally to TE0790/XMOD J2 F through hijack switch
GP9   JTAG TDO, normally to TE0790/XMOD J2 D; listen-only/shared is usually OK

GP10  JTAG_HIJACK, active high by default
GP11  DONE input, optional
GP12  INIT_B input, optional
```

TDO does not normally need switching: it is target-driven and both the Pico and
TE0790 can listen as inputs. The hijack switch should arbitrate TCK/TMS/TDI and
any other active control outputs.

## Build

On Ubuntu/Debian:

```bash
make deps
make
```

The default build is now FatFs-enabled and will clone:

- Raspberry Pi `pico-sdk` into `.deps/pico-sdk`
- `carlk3/no-OS-FatFS-SD-SPI-RPi-Pico` into `third_party/no-OS-FatFS-SD-SPI-RPi-Pico`

The important fix versus v0.2 is that this project now adds the FatFs backend
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
V                         version
L [path]                  list .BIT/.COR/.M65J files and dirs
I <file>                  inspect core file
P <file>                  hijack JTAG and program core
J                         read JTAG IDCODE, using hijack
H 1|0                     manually assert/release JTAG hijack
M                         mount/remount SD card
?                         help
```

Example:

```bash
python3 tools/uart_client.py /dev/ttyACM0 V
# or via the hardware UART:
python3 tools/uart_client.py /dev/ttyUSB0 V
python3 tools/uart_client.py /dev/ttyACM0 L /
python3 tools/uart_client.py /dev/ttyACM0 I /cores/mega65r6.cor
python3 tools/uart_client.py /dev/ttyACM0 P /cores/mega65r6.cor
```

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

## v1.2 Xilinx status / close-out diagnostics

This build keeps the v1.1 conservative post-configuration close-out and adds
Xilinx configuration-register diagnostics via JTAG `CFG_IN`/`CFG_OUT`.

New command:

```text
X                         read JTAG IDCODE plus Xilinx BOOTSTS/STAT/BYPASS
```

After `P <file>` or `stream <local.bit>`, the firmware now prints a diagnostic
line before the terminal `OK ... DONE` line:

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
