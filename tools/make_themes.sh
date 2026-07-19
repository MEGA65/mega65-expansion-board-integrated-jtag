#!/bin/bash
set -e

THEMES_DIR="$(dirname "$0")/../themes"
OUTPUT_DIR="$(dirname "$0")/../build-themes"

mkdir -p "$OUTPUT_DIR"

cd "$THEMES_DIR"
for theme in *; do
    if [ -d "$theme" ]; then
        echo "Building $theme theme..."
        cd "$theme"
        tar -cf "../../build-themes/mega65-jtag-$theme-theme.tar" .
        cd ..
    fi
done

echo "Themes built in build-themes/"
