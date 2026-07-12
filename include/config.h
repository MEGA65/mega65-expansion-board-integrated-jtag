#pragma once

#include "hardware/uart.h"
#include "hardware/spi.h"

// Command/control UART on the TE0790/XMOD socket. 2-wire 8N1 at 2 Mbps.
// The firmware also listens on Pico USB CDC by default, so this UART is optional
// for PC control but useful for MEGA65-side/native control.
#define M65_UART_ID          uart0
#define M65_UART_BAUD        2000000u
#define M65_UART_TX_PIN      0u
#define M65_UART_RX_PIN      1u

// Also accept commands on the Pico's native USB CDC serial port.
// CMake enables pico_stdio_usb(); this switch controls the command parser use.
#define M65_ENABLE_USB_CDC   1

// Suggested SPI pins for microSD. Your board can override these here.
#define M65_SD_SPI_ID        spi0
#define M65_SD_SCK_PIN       2u
#define M65_SD_MOSI_PIN      3u
#define M65_SD_MISO_PIN      4u
#define M65_SD_CS_PIN        5u
// #define M65_SD_SPI_BAUD      12500000u
#define M65_SD_SPI_BAUD      33000000u

// Pico-side JTAG pins.
#define M65_JTAG_TCK_PIN     6u
#define M65_JTAG_TMS_PIN     7u
#define M65_JTAG_TDI_PIN     8u
#define M65_JTAG_TDO_PIN     9u

// External mux/switch control. Active high by default: 1 = Pico owns JTAG.
#define M65_JTAG_HIJACK_PIN  10u
#define M65_JTAG_HIJACK_ACTIVE_HIGH 1

// Optional status pins. Set to 255 to disable if not wired.
#define M65_JTAG_DONE_PIN    255u
#define M65_JTAG_INIT_B_PIN  255u

#define M65_LED_PIN          PICO_DEFAULT_LED_PIN

// Conservative default. Set to 0 for fastest CPU bit-bang.
// This is a loop count, not an exact frequency. Use a scope and tune it.
#define M65_JTAG_DELAY_LOOPS 0u

// Conservative JTAG configuration close-out. Some bitstreams appear to need
// more post-JSTART clocks before the FPGA has fully completed startup. These
// are intentionally generous for bring-up; trim later once behaviour is known.
#define M65_JTAG_POST_JSTART_IDLE_CLOCKS     100000u
#define M65_JTAG_POST_ISC_NOOP_IDLE_CLOCKS   4096u
#define M65_JTAG_POST_BYPASS_IDLE_CLOCKS     1024u
#define M65_JTAG_POST_TAP_RESET_IDLE_CLOCKS  1024u

#define M65_VERSION_STRING   "pico-m65jtag 1.2-xstatus"
