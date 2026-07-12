# Pico to MEGA65 / TE0790-XMOD socket pinout

This firmware assumes the Pico is pretending to be, or sitting alongside, the
TE0790/XMOD connection. The target-side socket pin names below are the TE0790
J2/XMOD pin names from the standard XMOD firmware mapping.

## Pico firmware pins

| Pico GPIO | Firmware signal | Direction at Pico | Suggested target/socket connection |
|---:|---|---|---|
| GP0 | UART TX | output | J2 **B** / UART TXD output from adapter, to target RX |
| GP1 | UART RX | input | J2 **A** / UART RXD input to adapter, from target TX |
| GP2 | SD SCK | output | microSD SCK |
| GP3 | SD MOSI | output | microSD MOSI / CMD in SPI mode |
| GP4 | SD MISO | input | microSD MISO / DAT0 in SPI mode |
| GP5 | SD CS | output | microSD CS |
| GP6 | JTAG TCK | output | J2 **C** / TCK, through hijack switch |
| GP7 | JTAG TMS | output | J2 **H** / TMS, through hijack switch |
| GP8 | JTAG TDI | output | J2 **F** / TDI, through hijack switch |
| GP9 | JTAG TDO | input | J2 **D** / TDO; can normally be shared/listened to |
| GP10 | JTAG_HIJACK | output | control input of external JTAG mux/switch |
| GP11 | DONE | input | FPGA DONE, optional; set to 255 in config.h if absent |
| GP12 | INIT_B | input | FPGA INIT_B, optional; set to 255 in config.h if absent |

Also connect Pico GND to J2 pin **1/GND**. Do not power the Pico from arbitrary
JTAG pins unless you have deliberately designed the power path. The Pico is 3.3 V
only.

## Switching

Only the actively-driven adapter outputs need mandatory switching:

- TCK
- TMS
- TDI

TDO is target-driven, so both the TE0790 and the Pico can usually listen to it.
Use a small series resistor if you want cheap protection against mistakes or
unpowered-input leakage.

The switch should default to TE0790 passthrough when the Pico is unpowered,
reset, or not asserting `JTAG_HIJACK`.

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
