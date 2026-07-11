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
set(FATFS_SD_SPI_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/no-OS-FatFS-SD-SPI-RPi-Pico" CACHE PATH "Path to no-OS-FatFS-SD-SPI-RPi-Pico repository")

add_executable(pico_m65jtag
    src/main.c
    src/uart_cmd.c
    src/jtag_gpio.c
    src/core_file.c
)

target_include_directories(pico_m65jtag PRIVATE include)

target_link_libraries(pico_m65jtag
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_timer
    hardware_spi
)

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

    if(TARGET FatFs_SPI)
        target_link_libraries(pico_m65jtag FatFs_SPI)
    elseif(TARGET fatfs_spi)
        target_link_libraries(pico_m65jtag fatfs_spi)
    elseif(TARGET fatfs_sd_spi)
        target_link_libraries(pico_m65jtag fatfs_sd_spi)
    else()
        message(FATAL_ERROR "FatFs backend was added, but no known CMake target was exported. Expected FatFs_SPI.")
    endif()
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
