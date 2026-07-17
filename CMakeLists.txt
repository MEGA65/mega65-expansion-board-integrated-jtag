cmake_minimum_required(VERSION 3.13)

# Do not vendor pico_sdk_import.cmake. The Makefile passes PICO_SDK_PATH.
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Raspberry Pi Pico SDK")
    endif()
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Use the top-level Makefile or pass -DPICO_SDK_PATH=/path/to/pico-sdk")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(pico_m65jtag C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(M65_USE_FATFS "Build with FatFs storage backend" ON)
option(M65_HAVE_SD_INIT_DRIVER "Call sd_init_driver() before f_mount(); usually not needed for recent carlk3 library" OFF)
option(M65_ENABLE_WIFI_REMOTE "Build Pico W WiFi remote-delivery support" OFF)
set(M65_SD_MODE "" CACHE STRING "Optional compile-time SD mode override: M65_SD_MODE_AUTO, M65_SD_MODE_HW_SPI, or M65_SD_MODE_SCHEMATIC_BITBANG")
set(FATFS_SD_SPI_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/no-OS-FatFS-SD-SPI-RPi-Pico" CACHE PATH "Path to no-OS-FatFS-SD-SPI-RPi-Pico repository")

add_executable(pico_m65jtag
    src/bootsel_button.c
    src/main.c
    src/uart_cmd.c
    src/jtag_gpio.c
    src/core_file.c
    src/core_filter.c
    src/machine_identity.c
    src/remote_auth.c
    src/remote_http.c
    src/signed_file.c
    src/usb_descriptors.c
    src/write_gate.c
)

target_include_directories(pico_m65jtag PRIVATE include)
target_include_directories(pico_m65jtag PRIVATE
    "${PICO_SDK_PATH}/lib/mbedtls/include"
    "${PICO_SDK_PATH}/lib/mbedtls/include/mbedtls"
    "${PICO_SDK_PATH}/lib/mbedtls/library"
)
target_compile_definitions(pico_m65jtag PRIVATE
    MBEDTLS_CONFIG_FILE="m65_mbedtls_config.h"
    M65_PICO_BOARD_NAME="${PICO_BOARD}"
    USBD_MANUFACTURER="MEGA65"
    USBD_PRODUCT="MEGA65 Expansion Board Integrated JTAG"
    USBD_DESC_STR_MAX=64
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
)
if(M65_SD_MODE)
    target_compile_definitions(pico_m65jtag PRIVATE M65_SD_MODE=${M65_SD_MODE})
endif()

target_link_libraries(pico_m65jtag
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_timer
    hardware_spi
    hardware_watchdog
    pico_mbedtls_crypto
)

if(M65_ENABLE_WIFI_REMOTE)
    if(NOT PICO_CYW43_SUPPORTED)
        message(FATAL_ERROR "M65_ENABLE_WIFI_REMOTE requires a Pico W-class board, e.g. -DPICO_BOARD=pico_w")
    endif()
    target_compile_definitions(pico_m65jtag PRIVATE M65_WIFI_SUPPORTED=1)
    target_link_libraries(pico_m65jtag pico_cyw43_arch_lwip_poll)
else()
    target_compile_definitions(pico_m65jtag PRIVATE M65_WIFI_SUPPORTED=0)
endif()

if(M65_USE_FATFS)
    target_sources(pico_m65jtag PRIVATE
        src/storage_fatfs.c
        src/hw_config.c
    )
    target_compile_definitions(pico_m65jtag PRIVATE M65_USE_FATFS=1)
    if(M65_HAVE_SD_INIT_DRIVER)
        target_compile_definitions(pico_m65jtag PRIVATE HAVE_SD_INIT_DRIVER=1)
    endif()

    # carlk3/no-OS-FatFS-SD-SPI-RPi-Pico has no root CMakeLists.txt; the
    # library CMake project is under FatFs_SPI/. Accept either that repo root
    # or a direct path to FatFs_SPI for less painful local hacking.
    if(EXISTS "${FATFS_SD_SPI_PATH}/FatFs_SPI/CMakeLists.txt")
        set(FATFS_SPI_CMAKE_DIR "${FATFS_SD_SPI_PATH}/FatFs_SPI")
    elseif(EXISTS "${FATFS_SD_SPI_PATH}/CMakeLists.txt")
        set(FATFS_SPI_CMAKE_DIR "${FATFS_SD_SPI_PATH}")
    else()
        message(FATAL_ERROR "FatFs SPI backend not found at ${FATFS_SD_SPI_PATH}. Expected CMakeLists.txt at <repo>/FatFs_SPI/CMakeLists.txt. Run: make fatfs")
    endif()

    add_subdirectory("${FATFS_SPI_CMAKE_DIR}" fatfs_spi EXCLUDE_FROM_ALL)

    # The FatFs_SPI target normally publishes its include paths, but older
    # copies of the helper repo are inconsistent. Add the likely include roots.
    target_include_directories(pico_m65jtag PRIVATE
        "${FATFS_SPI_CMAKE_DIR}"
        "${FATFS_SPI_CMAKE_DIR}/sd_driver"
        "${FATFS_SPI_CMAKE_DIR}/ff14a/source"
        "${FATFS_SPI_CMAKE_DIR}/ff15/source"
        "${FATFS_SPI_CMAKE_DIR}/ff16/source"
        "${FATFS_SPI_CMAKE_DIR}/source"
    )

    target_sources(pico_m65jtag PRIVATE src/sd_transport.c)

    set(FATFS_SPI_TARGET "")
    if(TARGET FatFs_SPI)
        set(FATFS_SPI_TARGET FatFs_SPI)
    elseif(TARGET fatfs_spi)
        set(FATFS_SPI_TARGET fatfs_spi)
    elseif(TARGET fatfs_sd_spi)
        set(FATFS_SPI_TARGET fatfs_sd_spi)
    else()
        message(FATAL_ERROR "FatFs backend was added, but no known CMake target was exported. Expected FatFs_SPI.")
    endif()

    get_target_property(FATFS_SPI_SOURCES ${FATFS_SPI_TARGET} INTERFACE_SOURCES)
    if(FATFS_SPI_SOURCES)
        list(REMOVE_ITEM FATFS_SPI_SOURCES
            "${FATFS_SPI_CMAKE_DIR}/sd_driver/sd_spi.c"
            "${FATFS_SPI_CMAKE_DIR}/sd_driver/spi.c"
        )
        set_target_properties(${FATFS_SPI_TARGET} PROPERTIES INTERFACE_SOURCES "${FATFS_SPI_SOURCES}")
    endif()

    target_link_libraries(pico_m65jtag ${FATFS_SPI_TARGET})
else()
    target_sources(pico_m65jtag PRIVATE src/storage_stub.c)
    target_compile_definitions(pico_m65jtag PRIVATE M65_USE_FATFS=0)
endif()

# USB CDC is enabled so the same command protocol can be used over the Pico
# USB connector as well as over the hardware UART pins. Hardware UART is
# managed explicitly by src/uart_cmd.c rather than by stdio.
pico_enable_stdio_usb(pico_m65jtag 1)
pico_enable_stdio_uart(pico_m65jtag 0)


pico_add_extra_outputs(pico_m65jtag)
