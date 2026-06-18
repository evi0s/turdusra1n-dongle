#!/bin/bash
# Flash resource binaries to the RP2350B SPI flash at correct offsets
# Requires: picotool
#
# Flash layout:
#   0x140000: kpf.bin (128KB region)
#   0x160000: cpf.bin (128KB region)
#   0x180000: sep_racer.bin (1.5MB region)
#   0x300000: overlay.bin (1MB region)
#   0x400000: union.bin (2MB region)
#
# Note: Pongo_lz4.bin and stage1_a9.bin are embedded in firmware via .incbin
# Note: pteblock is stored at 0x600000 via slave mode (not flashed manually)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCE_DIR="$SCRIPT_DIR/resource"

if ! command -v picotool &> /dev/null; then
    echo "Error: picotool not found. Install from https://github.com/raspberrypi/picotool"
    exit 1
fi

echo "=== Turdus Dongle Resource Flash Tool ==="
echo ""

for f in kpf.bin cpf.bin sep_racer.bin overlay.bin union.bin; do
    if [ ! -f "$RESOURCE_DIR/$f" ]; then
        echo "Error: $RESOURCE_DIR/$f not found!"
        exit 1
    fi
    echo "Found: $f ($(stat -f%z "$RESOURCE_DIR/$f") bytes)"
done

echo ""
echo "Writing resources to flash..."
echo ""

echo "[1/5] Writing kpf.bin at flash offset 0x140000..."
picotool load "$RESOURCE_DIR/kpf.bin" -t bin -o 0x10140000 --ignore-partitions

echo "[2/5] Writing cpf.bin at flash offset 0x160000..."
picotool load "$RESOURCE_DIR/cpf.bin" -t bin -o 0x10160000 --ignore-partitions

echo "[3/5] Writing sep_racer.bin at flash offset 0x180000..."
picotool load "$RESOURCE_DIR/sep_racer.bin" -t bin -o 0x10180000 --ignore-partitions

echo "[4/5] Writing overlay.bin at flash offset 0x300000..."
picotool load "$RESOURCE_DIR/overlay.bin" -t bin -o 0x10300000 --ignore-partitions

echo "[5/5] Writing union.bin at flash offset 0x400000..."
picotool load "$RESOURCE_DIR/union.bin" -t bin -o 0x10400000 --ignore-partitions

echo ""
echo "Done! All resources written to flash."
echo ""
echo "To flash firmware: copy build/tethered_booter.uf2 to RP2350 in BOOTSEL mode"
