# Top-level convenience Makefile for MEGA65 Pico JTAG.
# Normal use:
#   make deps
#   make
#   make flash
#
# This wraps the Pico SDK CMake build, but keeps the boring CMake incantations
# out of day-to-day use.

SHELL := /bin/bash

PROJECT          ?= mega65-pico-jtag
CMAKE_TARGET     ?= pico_m65jtag
BUILD_DIR        ?= build-wifi
DEPS_DIR         ?= .deps
PICO_BOARD       ?= pico_w
PICO_SDK_PATH    ?= $(CURDIR)/$(DEPS_DIR)/pico-sdk
PICO_SDK_REPO    ?= https://github.com/raspberrypi/pico-sdk.git
PICOTOOL_REPO    ?= https://github.com/raspberrypi/picotool.git
PICOTOOL_PATH    ?= $(CURDIR)/$(DEPS_DIR)/picotool/build/picotool
PICOTOOL_FETCH_FROM_GIT_PATH ?= $(if $(wildcard $(CURDIR)/build/_deps/picotool/picotool),$(CURDIR)/build/_deps,)
WIFI_PROBE_BUILD_DIR ?= build-wifi-probe
WIFI_PROBE_UF2 ?= $(WIFI_PROBE_BUILD_DIR)/mega65-wifi-probe.uf2
MIRROR_DIR ?= mirror
MIRROR_CHANNEL ?= stable
MIRROR_BOARD ?= all
MIRROR_SOURCE_URL ?= https://files.mega65.org
MIRROR_EXTRA_CORES ?= $(wildcard extra_cores)
MIRROR_FIRMWARE ?= $(UF2)
MIRROR_THEME ?= $(MIRROR_DIR)/THEMES/mega65-jtag-default-theme.m65jtheme
MIRROR_THEME_NAME ?= default
MIRROR_EXTRA_THEMES ?=
MIRROR_VERSION ?= v0.1
WWW_THEME_FILES := app.js favicon-32x32.png fetch_busy.html index_bottom.html index_row.html index_top.html mega65_320x64.png style.css

# Default is now FatFs-on, because P <filename> and L are the whole point.
# Use `make nofatfs` or `make USE_FATFS=0` for a JTAG/UART-only bring-up build.
USE_FATFS        ?= 1
FATFS_REPO       ?= https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico.git
FATFS_PATH       ?= $(CURDIR)/third_party/no-OS-FatFS-SD-SPI-RPi-Pico
M65_HAVE_SD_INIT_DRIVER ?= 0
ENABLE_WIFI_REMOTE ?= 1
M65_SD_MODE ?=

UF2 := $(BUILD_DIR)/$(PROJECT).uf2
ELF := $(BUILD_DIR)/$(PROJECT).elf
CMAKE_UF2 := $(BUILD_DIR)/$(CMAKE_TARGET).uf2
CMAKE_ELF := $(BUILD_DIR)/$(CMAKE_TARGET).elf

ifeq ($(USE_FATFS),1)
CMAKE_FATFS_ARGS := -DM65_USE_FATFS=ON \
                    -DM65_HAVE_SD_INIT_DRIVER=$(if $(filter 1 ON on true TRUE yes YES,$(M65_HAVE_SD_INIT_DRIVER)),ON,OFF) \
                    -DFATFS_SD_SPI_PATH=$(FATFS_PATH)
CONFIGURE_DEPS := sdk fatfs
else
CMAKE_FATFS_ARGS := -DM65_USE_FATFS=OFF
CONFIGURE_DEPS := sdk
endif

CMAKE_REMOTE_ARGS := -DM65_ENABLE_WIFI_REMOTE=$(if $(filter 1 ON on true TRUE yes YES,$(ENABLE_WIFI_REMOTE)),ON,OFF)
CMAKE_SD_MODE_ARGS := $(if $(strip $(M65_SD_MODE)),-DM65_SD_MODE=$(M65_SD_MODE),)
CMAKE_PICOTOOL_ARGS := $(if $(strip $(PICOTOOL_FETCH_FROM_GIT_PATH)),-DPICOTOOL_FETCH_FROM_GIT_PATH=$(PICOTOOL_FETCH_FROM_GIT_PATH),)

.PHONY: all help deps check-tools sdk fatfs configure build nofatfs clean distclean nuke \
        upload flash upload-picotool upload-uf2 picotool print-config terminal \
        mirror mirror-setup \
        wifi-probe wifi-probe-lwip wifi-probe-bg wifi-probe-manual wifi-probe-manual-bg upload-wifi-probe

all: build

help:
	@echo "MEGA65 Pico JTAG Makefile"
	@echo
	@echo "Main targets:"
	@echo "  make deps              Install Ubuntu/Debian build dependencies with apt"
	@echo "  make                   Clone SDK/FatFs if needed, configure, and build UF2"
	@echo "  make flash             Flash the Pico; alias for make upload"
	@echo "  make upload            Try picotool first, then UF2 mass-storage copy"
	@echo "  make upload-uf2        Copy UF2 to mounted RPI-RP2/RP2350 BOOTSEL drive"
	@echo "  make picotool          Build local picotool under .deps/picotool/build/"
	@echo "  make upload-picotool   Flash using system/local picotool"
	@echo "  make nofatfs           Build UART/JTAG-only firmware without SD/FatFs"
	@echo "  make mirror            Build firmware, pack WWW theme, and prepare signed mirror/"
	@echo "  make wifi-probe        Build standalone Pico W bare CYW43 probe, no SD/FatFs/JTAG/lwIP"
	@echo "  make wifi-probe-lwip   Build same probe against pico_cyw43_arch_lwip_poll"
	@echo "  make wifi-probe-bg     Build same probe against pico_cyw43_arch_lwip_threadsafe_background"
	@echo "  make wifi-probe-manual Build staged manual CYW43/lwIP poll probe"
	@echo "  make wifi-probe-manual-bg Build staged manual CYW43/lwIP background probe"
	@echo "  make clean             Clean CMake build products"
	@echo "  make distclean         Remove build directory"
	@echo "  make nuke              Remove build directory and downloaded .deps"
	@echo
	@echo "Useful variables:"
	@echo "  USE_FATFS=1            Default. Build with FatFs/SPI-SD backend"
	@echo "  USE_FATFS=0            Build with storage stub only"
	@echo "  PICO_BOARD=pico_w      Default. Pico SDK board name: pico, pico_w, pico2, ..."
	@echo "  PICO_SDK_PATH=...      Override SDK path"
	@echo "  BUILD_DIR=build-wifi   Override build directory"
	@echo "  ENABLE_WIFI_REMOTE=1   Default. Set 0 for non-WiFi builds"
	@echo "  M65_SD_MODE=...        Optional: M65_SD_MODE_AUTO, M65_SD_MODE_HW_SPI, M65_SD_MODE_SCHEMATIC_BITBANG"
	@echo "  MIRROR_CHANNEL=stable  Mirror release channel/manifest prefix"
	@echo "  MIRROR_BOARD=all       Mirror board filter: all means both r3 and r6"
	@echo "  MIRROR_SOURCE_URL=...  Source catalogue URL for make mirror"
	@echo "  MIRROR_EXTRA_CORES=... Extra local .bit/.cor/.m65j files/dirs; default extra_cores/ if present"
	@echo "  MIRROR_FIRMWARE=...    Firmware image staged into mirror; default built UF2"
	@echo "  MIRROR_EXTRA_THEMES=... Extra local .m65jtheme/.tar files to publish under THEMES/"
	@echo "  PICO_MOUNT=...         Mount point for upload-uf2, if autodetect fails"
	@echo
	@echo "Examples:"
	@echo "  make deps"
	@echo "  make"
	@echo "  make flash"
	@echo "  make PICO_BOARD=pico ENABLE_WIFI_REMOTE=0 BUILD_DIR=build build"
	@echo "  make USE_FATFS=0 build"

# Ubuntu/Debian dependency install. This deliberately does not clone the SDK;
# the build does that via `make sdk` so the path is predictable.
deps:
	sudo apt update
	sudo apt install -y \
		build-essential cmake git python3 pkg-config \
		gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
		libusb-1.0-0-dev python3-serial

check-tools:
	@command -v cmake >/dev/null || { echo "missing cmake; run make deps"; exit 1; }
	@command -v git >/dev/null || { echo "missing git; run make deps"; exit 1; }
	@command -v arm-none-eabi-gcc >/dev/null || { echo "missing arm-none-eabi-gcc; run make deps"; exit 1; }
	@command -v python3 >/dev/null || { echo "missing python3; run make deps"; exit 1; }
	@echo "Toolchain basics look present."

sdk: check-tools
	@if [ ! -d "$(PICO_SDK_PATH)/.git" ]; then \
		echo "Cloning pico-sdk into $(PICO_SDK_PATH)"; \
		mkdir -p "$(dir $(PICO_SDK_PATH))"; \
		git clone --recurse-submodules "$(PICO_SDK_REPO)" "$(PICO_SDK_PATH)"; \
	else \
		echo "Using existing pico-sdk at $(PICO_SDK_PATH)"; \
		git -C "$(PICO_SDK_PATH)" submodule update --init --recursive; \
	fi

fatfs:
	@if [ ! -d "$(FATFS_PATH)/.git" ]; then \
		echo "Cloning Pico SPI-SD FatFs backend into $(FATFS_PATH)"; \
		mkdir -p "$(dir $(FATFS_PATH))"; \
		git clone --recurse-submodules "$(FATFS_REPO)" "$(FATFS_PATH)"; \
	else \
		echo "Using existing FatFs backend at $(FATFS_PATH)"; \
		git -C "$(FATFS_PATH)" submodule update --init --recursive || true; \
	fi
	@if [ ! -f "$(FATFS_PATH)/FatFs_SPI/CMakeLists.txt" ]; then \
		echo "ERROR: FatFs backend clone exists, but $(FATFS_PATH)/FatFs_SPI/CMakeLists.txt is missing."; \
		echo "Try: rm -rf $(FATFS_PATH) && make fatfs"; \
		exit 1; \
	fi

configure: $(CONFIGURE_DEPS)
	cmake -S . -B "$(BUILD_DIR)" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD="$(PICO_BOARD)" \
		$(CMAKE_FATFS_ARGS) \
		$(CMAKE_REMOTE_ARGS) \
		$(CMAKE_SD_MODE_ARGS) \
		$(CMAKE_PICOTOOL_ARGS)

build: configure
	cmake --build "$(BUILD_DIR)" --parallel
	@if [ "$(PROJECT)" != "$(CMAKE_TARGET)" ]; then \
		cp "$(CMAKE_UF2)" "$(UF2)"; \
		cp "$(CMAKE_ELF)" "$(ELF)"; \
	fi
	@echo
	@echo "Built: $(UF2)"

mirror: build
	@set -e; \
	mkdir -p "$(MIRROR_DIR)"; \
	mkdir -p "$(MIRROR_DIR)/WWW" "$(dir $(MIRROR_THEME))"; \
	echo "Copying default SD web assets into $(MIRROR_DIR)/WWW"; \
	for asset in $(WWW_THEME_FILES); do \
		cp "sdcard/WWW/$$asset" "$(MIRROR_DIR)/WWW/$$asset"; \
	done; \
	cp sdcard/mega65-jtag.cfg "$(MIRROR_DIR)/mega65-jtag.cfg.example"; \
	echo "Packing default WWW theme: $(MIRROR_THEME)"; \
	tar -cf "$(MIRROR_THEME)" -C sdcard/WWW $(WWW_THEME_FILES); \
	build_marker="$$(sed -n 's/^#define M65_BUILD_MARKER "\(.*\)"/\1/p' "$(BUILD_DIR)/generated/generated_version.h")"; \
	if [ -z "$$build_marker" ]; then \
		echo "ERROR: could not read M65_BUILD_MARKER from $(BUILD_DIR)/generated/generated_version.h"; \
		exit 1; \
	fi; \
	extra_args=(); \
	extra_items="$(strip $(MIRROR_EXTRA_CORES))"; \
	if [ -n "$$extra_items" ]; then \
		echo "Including extra local cores: $$extra_items"; \
		for extra in $$extra_items; do \
			extra_args+=(--extra-core "$$extra"); \
		done; \
	fi; \
	theme_args=(); \
	theme_items="$(strip $(MIRROR_EXTRA_THEMES))"; \
	if [ -n "$$theme_items" ]; then \
		echo "Including extra local themes: $$theme_items"; \
		for theme in $$theme_items; do \
			theme_args+=(--theme "$$theme"); \
		done; \
	fi; \
	echo "Staging firmware image: $(MIRROR_FIRMWARE)"; \
	echo "Preparing $(MIRROR_CHANNEL) mirror in $(MIRROR_DIR) for board(s) $(MIRROR_BOARD) with firmware build $$build_marker"; \
	python3 tools/m65j.py mirror \
		--board "$(MIRROR_BOARD)" \
		--overwrite \
		--bless \
		--yes \
		--firmware "$(MIRROR_FIRMWARE)" \
		--firmware-version "$(MIRROR_VERSION)" \
		--firmware-build "$$build_marker" \
		--theme "$(MIRROR_THEME)" \
		--theme-name "$(MIRROR_THEME_NAME)" \
		--theme-version "$$build_marker" \
		"$${theme_args[@]}" \
		"$${extra_args[@]}" \
		"$(MIRROR_CHANNEL)" "$(MIRROR_DIR)" "$(MIRROR_SOURCE_URL)"

mirror-setup: mirror

nofatfs:
	$(MAKE) USE_FATFS=0 BUILD_DIR=build-nofatfs build

wifi-probe: sdk
	cmake -S wifi_probe -B "$(WIFI_PROBE_BUILD_DIR)" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD=pico_w \
		-DWIFI_PROBE_MODE=none \
		$(CMAKE_PICOTOOL_ARGS)
	cmake --build "$(WIFI_PROBE_BUILD_DIR)" --parallel
	cp "$(WIFI_PROBE_BUILD_DIR)/m65_wifi_probe.uf2" "$(WIFI_PROBE_UF2)"
	@echo
	@echo "Built: $(WIFI_PROBE_UF2)"

wifi-probe-lwip: sdk
	cmake -S wifi_probe -B "$(WIFI_PROBE_BUILD_DIR)-lwip" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD=pico_w \
		-DWIFI_PROBE_MODE=lwip_poll \
		$(CMAKE_PICOTOOL_ARGS)
	cmake --build "$(WIFI_PROBE_BUILD_DIR)-lwip" --parallel
	cp "$(WIFI_PROBE_BUILD_DIR)-lwip/m65_wifi_probe.uf2" "$(WIFI_PROBE_BUILD_DIR)-lwip/mega65-wifi-probe-lwip.uf2"
	@echo
	@echo "Built: $(WIFI_PROBE_BUILD_DIR)-lwip/mega65-wifi-probe-lwip.uf2"

wifi-probe-bg: sdk
	cmake -S wifi_probe -B "$(WIFI_PROBE_BUILD_DIR)-bg" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD=pico_w \
		-DWIFI_PROBE_MODE=lwip_background \
		$(CMAKE_PICOTOOL_ARGS)
	cmake --build "$(WIFI_PROBE_BUILD_DIR)-bg" --parallel
	cp "$(WIFI_PROBE_BUILD_DIR)-bg/m65_wifi_probe.uf2" "$(WIFI_PROBE_BUILD_DIR)-bg/mega65-wifi-probe-bg.uf2"
	@echo
	@echo "Built: $(WIFI_PROBE_BUILD_DIR)-bg/mega65-wifi-probe-bg.uf2"

wifi-probe-manual: sdk
	cmake -S wifi_probe -B "$(WIFI_PROBE_BUILD_DIR)-manual" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD=pico_w \
		-DWIFI_PROBE_MODE=manual_poll \
		$(CMAKE_PICOTOOL_ARGS)
	cmake --build "$(WIFI_PROBE_BUILD_DIR)-manual" --parallel
	cp "$(WIFI_PROBE_BUILD_DIR)-manual/m65_wifi_probe.uf2" "$(WIFI_PROBE_BUILD_DIR)-manual/mega65-wifi-probe-manual.uf2"
	@echo
	@echo "Built: $(WIFI_PROBE_BUILD_DIR)-manual/mega65-wifi-probe-manual.uf2"

wifi-probe-manual-bg: sdk
	cmake -S wifi_probe -B "$(WIFI_PROBE_BUILD_DIR)-manual-bg" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
		-DPICO_BOARD=pico_w \
		-DWIFI_PROBE_MODE=manual_background \
		$(CMAKE_PICOTOOL_ARGS)
	cmake --build "$(WIFI_PROBE_BUILD_DIR)-manual-bg" --parallel
	cp "$(WIFI_PROBE_BUILD_DIR)-manual-bg/m65_wifi_probe.uf2" "$(WIFI_PROBE_BUILD_DIR)-manual-bg/mega65-wifi-probe-manual-bg.uf2"
	@echo
	@echo "Built: $(WIFI_PROBE_BUILD_DIR)-manual-bg/mega65-wifi-probe-manual-bg.uf2"

upload-wifi-probe: wifi-probe
	@set -e; \
	if [ ! -f "$(WIFI_PROBE_UF2)" ]; then echo "No UF2 at $(WIFI_PROBE_UF2)"; exit 1; fi; \
	mount="$$PICO_MOUNT"; \
	if [ -z "$$mount" ]; then \
		for d in \
			/media/$$USER/RPI-RP2 \
			/run/media/$$USER/RPI-RP2 \
			/mnt/RPI-RP2 \
			/Volumes/RPI-RP2 \
			/media/$$USER/RP2350 \
			/run/media/$$USER/RP2350 \
			/mnt/RP2350 \
			/Volumes/RP2350; do \
			if [ -d "$$d" ]; then mount="$$d"; break; fi; \
		done; \
	fi; \
	if [ -z "$$mount" ]; then \
		echo "Could not find RPI-RP2/RP2350 mount."; \
		echo "Hold BOOTSEL while plugging the Pico into USB, or set PICO_MOUNT=/path/to/RPI-RP2."; \
		exit 1; \
	fi; \
	echo "Copying $(WIFI_PROBE_UF2) to $$mount/"; \
	cp "$(WIFI_PROBE_UF2)" "$$mount/"; \
	sync; \
	echo "Flash requested. The Pico should reboot after the UF2 copy completes."

clean:
	@if [ -d "$(BUILD_DIR)" ]; then cmake --build "$(BUILD_DIR)" --target clean; fi

distclean:
	rm -rf "$(BUILD_DIR)" build-nofatfs "$(WIFI_PROBE_BUILD_DIR)" "$(WIFI_PROBE_BUILD_DIR)-lwip" "$(WIFI_PROBE_BUILD_DIR)-bg" "$(WIFI_PROBE_BUILD_DIR)-manual" "$(WIFI_PROBE_BUILD_DIR)-manual-bg"

nuke: distclean
	rm -rf "$(DEPS_DIR)"

picotool: sdk
	@if [ ! -d "$(DEPS_DIR)/picotool/.git" ]; then \
		echo "Cloning picotool into $(DEPS_DIR)/picotool"; \
		mkdir -p "$(DEPS_DIR)"; \
		git clone "$(PICOTOOL_REPO)" "$(DEPS_DIR)/picotool"; \
	fi
	cmake -S "$(DEPS_DIR)/picotool" -B "$(DEPS_DIR)/picotool/build" \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)"
	cmake --build "$(DEPS_DIR)/picotool/build" --parallel
	@echo "Built: $(PICOTOOL_PATH)"

flash: upload

upload: build
	@if command -v picotool >/dev/null 2>&1; then \
		echo "Trying system picotool..."; \
		sudo picotool load -x "$(UF2)" || { echo "picotool failed; trying UF2 copy"; $(MAKE) upload-uf2 BUILD_DIR="$(BUILD_DIR)" PROJECT="$(PROJECT)"; }; \
	elif [ -x "$(PICOTOOL_PATH)" ]; then \
		echo "Trying local $(PICOTOOL_PATH)..."; \
		sudo "$(PICOTOOL_PATH)" load -x "$(UF2)" || { echo "picotool failed; trying UF2 copy"; $(MAKE) upload-uf2 BUILD_DIR="$(BUILD_DIR)" PROJECT="$(PROJECT)"; }; \
	else \
		echo "picotool not found; trying UF2 mass-storage copy."; \
		$(MAKE) upload-uf2 BUILD_DIR="$(BUILD_DIR)" PROJECT="$(PROJECT)"; \
	fi

upload-picotool: build
	@if command -v picotool >/dev/null 2>&1; then \
		sudo picotool load -x "$(UF2)"; \
	elif [ -x "$(PICOTOOL_PATH)" ]; then \
		sudo "$(PICOTOOL_PATH)" load -x "$(UF2)"; \
	else \
		echo "No picotool found. Run 'make picotool' or install it with your distro."; \
		exit 1; \
	fi

upload-uf2: build
	@set -e; \
	if [ ! -f "$(UF2)" ]; then echo "No UF2 at $(UF2)"; exit 1; fi; \
	mount="$$PICO_MOUNT"; \
	if [ -z "$$mount" ]; then \
		for d in \
			/media/$$USER/RPI-RP2 \
			/run/media/$$USER/RPI-RP2 \
			/mnt/RPI-RP2 \
			/Volumes/RPI-RP2 \
			/media/$$USER/RP2350 \
			/run/media/$$USER/RP2350 \
			/mnt/RP2350 \
			/Volumes/RP2350; do \
			if [ -d "$$d" ]; then mount="$$d"; break; fi; \
		done; \
	fi; \
	if [ -z "$$mount" ]; then \
		echo "Could not find RPI-RP2/RP2350 mount."; \
		echo "Hold BOOTSEL while plugging the Pico into USB, or set PICO_MOUNT=/path/to/RPI-RP2."; \
		exit 1; \
	fi; \
	echo "Copying $(UF2) to $$mount/"; \
	cp "$(UF2)" "$$mount/"; \
	sync; \
	echo "Flash requested. The Pico should reboot after the UF2 copy completes."

print-config:
	@echo "PROJECT=$(PROJECT)"
	@echo "CMAKE_TARGET=$(CMAKE_TARGET)"
	@echo "BUILD_DIR=$(BUILD_DIR)"
	@echo "PICO_BOARD=$(PICO_BOARD)"
	@echo "PICO_SDK_PATH=$(PICO_SDK_PATH)"
	@echo "PICOTOOL_FETCH_FROM_GIT_PATH=$(PICOTOOL_FETCH_FROM_GIT_PATH)"
	@echo "USE_FATFS=$(USE_FATFS)"
	@echo "FATFS_PATH=$(FATFS_PATH)"
	@echo "ENABLE_WIFI_REMOTE=$(ENABLE_WIFI_REMOTE)"
	@echo "M65_SD_MODE=$(M65_SD_MODE)"
	@echo "UF2=$(UF2)"

terminal:
	@echo "Example command client:"
	@echo "  python3 tools/m65j.py /dev/ttyUSB0 ATI"
	@echo "or use picocom/minicom at 2000000 8N1."
