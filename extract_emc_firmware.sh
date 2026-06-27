#!/bin/bash
#
# extract_emc_firmware.sh — Extract EMC firmware from PS5 serial flash dump
#
# Usage: ./extract_emc_firmware.sh <sflash0.bin> <output_dir>
#
# Steps:
#   1. Extract SLB2 container from offset 0x4000 (size 0x7E000)
#   2. Unpack SLB2 with blsunpack
#   3. List extracted files
#
# Prerequisites:
#   - sflash0.bin: 2MB serial flash dump from PS5 (via sflash_dump.elf payload)
#   - blsunpack: built at /Users/bizkut/Downloads/PS5/homebrew/blsunpack/blsunpack
#   - Docker for running blsunpack (x86-64 Linux binary)
#

set -e

BLSUNPACK_DIR="/Users/bizkut/Downloads/PS5/homebrew/blsunpack"
SFLASH_OFFSET=0x4000      # EMC firmware SLB2 container offset in serial flash
SFLASH_SIZE=0x7E000       # EMC firmware SLB2 container size (508KB)

if [ $# -lt 2 ]; then
    echo "Usage: $0 <sflash0.bin> <output_dir>"
    echo ""
    echo "  sflash0.bin : 2MB serial flash dump from PS5"
    echo "  output_dir  : directory for extracted files"
    exit 1
fi

SFLASH_BIN="$1"
OUTDIR="$2"

if [ ! -f "$SFLASH_BIN" ]; then
    echo "Error: $SFLASH_BIN not found"
    exit 1
fi

FILESIZE=$(stat -f%z "$SFLASH_BIN" 2>/dev/null || stat -c%s "$SFLASH_BIN" 2>/dev/null)
echo "Input: $SFLASH_BIN ($FILESIZE bytes)"

if [ "$FILESIZE" -lt 524288 ]; then
    echo "Error: file too small (expected at least 512KB, got $FILESIZE)"
    exit 1
fi

mkdir -p "$OUTDIR"

# Step 1: Extract SLB2 container
echo ""
echo "=== Step 1: Extract SLB2 container from offset 0x4000 ==="
SLB2_FILE="$OUTDIR/emc_fw.slb2"
dd if="$SFLASH_BIN" of="$SLB2_FILE" bs=1 skip=$((SFLASH_OFFSET)) count=$((SFLASH_SIZE)) 2>&1

# Verify SLB2 magic
MAGIC=$(xxd -l 4 "$SLB2_FILE" | head -1)
echo "First 4 bytes: $MAGIC"

# Check for SLB2 magic (53 4c 42 32 = "SLB2")
SIG=$(xxd -l 4 -p "$SLB2_FILE")
if [ "$SIG" = "534c4232" ]; then
    echo "SLB2 magic found!"
elif [ "$SIG" = "32424c53" ]; then
    echo "SLB2 magic found (reversed byte order)!"
else
    echo "Warning: SLB2 magic not found (got $SIG)"
    echo "First 64 bytes:"
    xxd -l 64 "$SLB2_FILE"
    echo ""
    echo "Trying anyway..."
fi

# Step 2: Unpack with blsunpack
echo ""
echo "=== Step 2: Unpack SLB2 with blsunpack ==="
SLB2_DIR="$OUTDIR/emc_fw_unpacked"
mkdir -p "$SLB2_DIR"

docker run --rm --platform linux/amd64 \
    -v "$BLSUNPACK_DIR:/blsunpack:ro" \
    -v "$(cd "$OUTDIR" && pwd):/work" \
    ubuntu:24.04 \
    bash -c "cd /work && /blsunpack/blsunpack emc_fw.slb2 emc_fw_unpacked" 2>&1 || {
    echo ""
    echo "blsunpack failed. Trying with different block sizes..."
    # The SLB2 header might use different block sizes
    # Try extracting the TOC manually
    echo "First 256 bytes of SLB2:"
    xxd -l 256 "$SLB2_FILE"
}

# Step 3: List extracted files
echo ""
echo "=== Step 3: Extracted files ==="
if [ -d "$SLB2_DIR" ]; then
    find "$SLB2_DIR" -type f -exec ls -la {} \; 2>/dev/null
    echo ""
    echo "File types:"
    find "$SLB2_DIR" -type f -exec file {} \; 2>/dev/null
else
    echo "No files extracted"
fi

# Step 4: Also extract NVS area for reference
echo ""
echo "=== Step 4: Extract NVS area (offset 0x1C4000) ==="
NVS_FILE="$OUTDIR/nvs.bin"
dd if="$SFLASH_BIN" of="$NVS_FILE" bs=1 skip=$((0x1C4000)) count=$((0x3C000)) 2>&1
echo "NVS area: $NVS_FILE"

# Step 5: Summary
echo ""
echo "=== Summary ==="
echo "Serial flash dump: $SFLASH_BIN"
echo "EMC firmware SLB2: $SLB2_FILE"
echo "Unpacked firmware: $SLB2_DIR"
echo "NVS area:          $NVS_FILE"
echo ""
echo "Next steps:"
echo "  1. Load extracted ELF files into Ghidra (ARM Cortex-M, little-endian)"
echo "  2. Look for ICC command handlers (service_id dispatch table)"
echo "  3. Find USB-C DP alt mode handling (service_id=0x12)"
echo "  4. Find HDMI/EDID handling (service_id=0x10)"
echo "  5. Find HPD notification and redriver config"
