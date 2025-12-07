#!/bin/bash
# Extract binary ROM from Arduino PROGMEM header
# Usage: ./extract_rom.sh input.ino output.bin

INPUT=$1
OUTPUT=$2

if [ -z "$INPUT" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 input.ino output.bin"
    exit 1
fi

# Extract hex bytes from the rom_bin array and convert to binary
grep -A 10000 'rom_bin\[\]' "$INPUT" | \
    grep -o '0x[0-9a-fA-F][0-9a-fA-F]' | \
    sed 's/0x//' | \
    xxd -r -p > "$OUTPUT"

SIZE=$(wc -c < "$OUTPUT" | tr -d ' ')
echo "Extracted $SIZE bytes to $OUTPUT"
