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

// The modem command surface is AT-style by default. Set this to 1 only when
// you need the early bring-up one-letter protocol (`V`, `L`, `P`, etc.).
#ifndef M65_ENABLE_LEGACY_UART_COMMANDS
#define M65_ENABLE_LEGACY_UART_COMMANDS 0
#endif

// WiFi remote delivery is only built for Pico W-class boards when enabled in
// CMake. Runtime enablement still requires /mega65-jtag.cfg on the SD card.
#ifndef M65_WIFI_SUPPORTED
#define M65_WIFI_SUPPORTED   0
#endif

// SD transport runtime policy. Both layouts are built into the firmware:
//   M65_SD_MODE_HW_SPI             = legal RP2040 hardware-SPI pinout
//   M65_SD_MODE_SCHEMATIC_BITBANG  = first fabbed schematic pinout
// AUTO probes schematic-bitbang first, then hardware SPI. Use `AT+SDMODE=...`
// before mounting SD to override the detected/default mode at runtime.
#define M65_SD_MODE_AUTO                 0
#define M65_SD_MODE_HW_SPI               1
#define M65_SD_MODE_SCHEMATIC_BITBANG    2
#ifndef M65_SD_MODE
#define M65_SD_MODE                      M65_SD_MODE_AUTO
#endif
#define M65_SD_AUTO_FALLBACK_MODE        M65_SD_MODE_SCHEMATIC_BITBANG

#define M65_SD_HW_SPI_ID        spi0
#define M65_SD_HW_SCK_PIN       2u
#define M65_SD_HW_MOSI_PIN      3u
#define M65_SD_HW_MISO_PIN      4u
#define M65_SD_HW_CS_PIN        5u

#define M65_SD_SOFT_SCK_PIN     4u
#define M65_SD_SOFT_MOSI_PIN    3u
#define M65_SD_SOFT_MISO_PIN    5u
#define M65_SD_SOFT_CS_PIN      2u

#define M65_SD_SPI_ID           M65_SD_HW_SPI_ID
#define M65_SD_SCK_PIN          M65_SD_SOFT_SCK_PIN
#define M65_SD_MOSI_PIN         M65_SD_SOFT_MOSI_PIN
#define M65_SD_MISO_PIN         M65_SD_SOFT_MISO_PIN
#define M65_SD_CS_PIN           M65_SD_SOFT_CS_PIN
#define M65_SD_SPI_BAUD      30000000u
#define M65_SD_BITBANG_LOW_HALF_PERIOD_US 2u
#define M65_SD_BITBANG_FAST_DELAY_LOOPS   0u
#define M65_SD_PROBE_RETRIES              8u
#define M65_SD_PROBE_RETRY_DELAY_MS       5u

// Pico-side JTAG pins.
#define M65_JTAG_TCK_PIN     6u
#define M65_JTAG_TMS_PIN     7u
#define M65_JTAG_TDI_PIN     8u
#define M65_JTAG_TDO_PIN     9u

// External mux/switch control. Active high by default: 1 = Pico owns JTAG.
#define M65_JTAG_HIJACK_PIN  10u
#define M65_JTAG_HIJACK_ACTIVE_HIGH 1

// Physical write-enable authority input. Default wiring is a momentary button
// or removable jumper from GP11 to GND; the internal pull-up makes open = off.
// If this is 255, destructive SD write commands are disabled.
#define M65_WRITE_ENABLE_PIN             11u
#define M65_WRITE_ENABLE_ACTIVE_LOW      1
#define M65_WRITE_ENABLE_TIMEOUT_MS      120000u
#define M65_WRITE_COMMANDS_USB_ONLY      1
#define M65_STREAM_COMMANDS_USB_ONLY     1

// The Pico's onboard BOOTSEL button is not a normal GPIO; firmware can still
// poll it by briefly tri-stating flash CS from RAM. If a poll catches it
// pressed, the firmware reboots to UF2/BOOTSEL mode.
#define M65_BOOTSEL_BUTTON_POLL_MS       50u

// Give USB CDC a chance to enumerate before SD-backed settings and WiFi
// connection attempts can block boot progress.
#define M65_REMOTE_INIT_DELAY_MS         2000u
#define M65_WIFI_CONNECT_TIMEOUT_MS      10000u
#define M65_WIFI_FIRST_RETRY_DELAY_MS    5000u
#define M65_WIFI_PROBE_FAST_ATTEMPTS     4u
#define M65_WIFI_PROBE_FAST_RETRY_MS     1000u
#define M65_WIFI_NOIP_TIMEOUT_MS         10000u
#define M65_FETCH_CONNECT_TIMEOUT_MS     10000u
#define M65_FETCH_IDLE_TIMEOUT_MS        60000u
#define M65_FETCH_IDLE_DIAG_MS           3000u
#define M65_FETCH_ACK_NUDGE_MS           1000u
#define M65_AUTOFETCH_FILE_RETRIES       10u
#define M65_WIFI_COMMAND_QUIET_MS        15000u

#ifndef M65_LED_PIN
#ifdef PICO_DEFAULT_LED_PIN
#define M65_LED_PIN          PICO_DEFAULT_LED_PIN
#else
#define M65_LED_PIN          255u
#endif
#endif

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

#ifndef M65_PICO_BOARD_NAME
#define M65_PICO_BOARD_NAME "unknown"
#endif

#define M65_VERSION_STRING   "MEGA65 Expansion Board Integrated JTAG v0.1"
#define M65_BUILD_MARKER     "autofetch-keeps-wifi-20260718"
