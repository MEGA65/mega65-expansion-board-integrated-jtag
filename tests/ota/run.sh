#!/bin/sh
# Host test of the OTA staging (src/ota_apply.c) and install
# (bootloader/ota_install.c) code, including power cuts at every flash
# operation. Needs both factory builds and both plain builds:
#
#   make PICO_BOARD=pico_w  BUILD_DIR=build-picow factory build-bare
#   make PICO_BOARD=pico2_w BUILD_DIR=build-wifi  factory build-bare
#   tests/ota/run.sh build-picow build-wifi
set -eu
picow=${1:?pico_w build dir}
pico2w=${2:?pico2_w build dir}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/ota_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT

build() {
    cc -std=gnu11 -O2 -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
        -DPICO_RP2040="$1" -I"$here/stubs" -I"$root/include" \
        -o "$out/ota_test_$2" \
        "$here/ota_test.c" "$root/src/ota_apply.c" "$root/bootloader/ota_install.c"
}
build 1 rp2040
build 0 rp2350

"$out/ota_test_rp2040" "$picow/mega65-pico-jtag-factory.uf2" "$picow-factory/pico_m65jtag.bin" \
    "$pico2w/mega65-pico-jtag-factory.uf2" "$picow/pico_m65jtag.uf2"
"$out/ota_test_rp2350" "$pico2w/mega65-pico-jtag-factory.uf2" "$pico2w-factory/pico_m65jtag.bin" \
    "$picow/mega65-pico-jtag-factory.uf2" "$pico2w/pico_m65jtag.uf2"
