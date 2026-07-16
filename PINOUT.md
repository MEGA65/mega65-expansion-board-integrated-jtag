# MEGA65 Expansion Board Integrated JTAG Firmware & Utilities Pinout

**Status:** v0.1 experimental. The wiring, pin assignments, SD-card transport
selection, JTAG hijack arrangement, and UART command/control protocol are all
still subject to change.

This firmware currently assumes the Pico is pretending to be, or sitting
alongside, the TE0790/XMOD connection. The target-side socket pin names below
are the TE0790 J2/XMOD pin names from the standard XMOD firmware mapping.

## Pico Firmware Pins

Treat this table as the current firmware default, not a frozen hardware
interface. Check `include/config.h` before routing a PCB.

| Pico GPIO | Firmware signal | Direction at Pico | Suggested target/socket connection |
|---:|---|---|---|
| GP0 | UART TX | output | J2 **B** / UART TXD output from adapter, to target RX |
| GP1 | UART RX | input | J2 **A** / UART RXD input to adapter, from target TX |
| GP2 | SD CS | output | microSD CS |
| GP3 | SD MOSI | output | microSD MOSI / CMD in SPI mode |
| GP4 | SD SCK | output | microSD SCK |
| GP5 | SD MISO | input | microSD MISO / DAT0 in SPI mode |
| GP6 | JTAG TCK | output | J2 **C** / TCK, through hijack switch |
| GP7 | JTAG TMS | output | J2 **H** / TMS, through hijack switch |
| GP8 | JTAG TDI | output | J2 **F** / TDI, through hijack switch |
| GP9 | JTAG TDO | input | J2 **D** / TDO, through hijack switch if using a 4-bit mux |
| GP10 | JTAG_HIJACK | output | control input of external JTAG mux/switch |
| GP11 | WRITE_ENABLE | input | active-low physical write-authority button/jumper to GND |
| GP12 | spare | input | unused by default |

The firmware can also use the corrected hardware-SPI layout at runtime:
`GP2=SD SCK`, `GP3=SD MOSI`, `GP4=SD MISO`, `GP5=SD CS`. The default
`AUTO` mode probes the schematic bit-banged layout first, then this hardware
layout.

Also connect Pico GND to J2 pin **1/GND**. Do not power the Pico from arbitrary
JTAG pins unless you have deliberately designed the power path. The Pico is 3.3 V
only.

## Switching

For a clean four-channel hijack switch, switch all four JTAG lines:

- TCK
- TMS
- TDI
- TDO

The FPGA/target side should be the common side of the mux. The selectable sides
are the TE0790 path and the Pico path. The switch should default to TE0790
passthrough when the Pico is unpowered, reset, or not asserting `JTAG_HIJACK`.

## UART command/control

The Pico listens on two command ports by default:

1. GP0/GP1 at 2 Mbps 8N1, intended for target/MEGA65-side software control via
   the TE0790/XMOD UART pins.
2. Native Pico USB CDC serial, so a PC can talk to the Pico with no extra USB
   UART adapter.

Replies are broadcast to both ports.

If your TE0790 socket/firmware uses the "RXD-TXD swapped" mapping, swap the
J2 **A** and **B** UART connections. The JTAG pins are unchanged for the standard
and swapped-standard mappings.


## Write-enable input

By default, GP11 is a physical write-authority input. It uses the Pico internal
pull-up, so open = write protected and short-to-GND = write enabled. The firmware
latches write authority for `M65_WRITE_ENABLE_TIMEOUT_MS` after the input is
asserted, so a momentary button can be used instead of a permanent jumper.

Release firmware should leave `M65_WRITE_COMMANDS_USB_ONLY=1`, so the MEGA65-side
UART cannot write files even during a physical update window.
