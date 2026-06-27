#!/usr/bin/env python3
"""
Dump PS5 serial flash via the ps5_bar2_dump kernel module.

Reads from /dev/ps5_bar2 (or /sys/kernel/debug/ps5_bar2_dump)
and saves the 2MB BAR2 region.

Usage:
  python3 dump_bar2.py [output_file]

Prerequisites:
  - Build and load ps5_bar2_dump.ko on the PS5:
    make -f Makefile.bar2
    sudo insmod ps5_bar2_dump.ko
"""

import sys
import os

def dump(source, output, size=2*1024*1024, chunk=65536):
    print(f"Dumping {size} bytes from {source} to {output}...")
    with open(source, 'rb') as f_in, open(output, 'wb') as f_out:
        written = 0
        while written < size:
            to_read = min(chunk, size - written)
            data = f_in.read(to_read)
            if not data:
                print(f"  EOF at {written:#x}/{size:#x}")
                break
            f_out.write(data)
            written += len(data)
            if written % (256*1024) == 0:
                print(f"  {written:#08x}/{size:#08x} ({100*written//size}%)")
    print(f"Done: {written} bytes -> {output}")

    # Show first 256 bytes
    with open(output, 'rb') as f:
        data = f.read(256)
        print(f"\nFirst 256 bytes:")
        for i in range(0, len(data), 16):
            hex_str = ' '.join(f'{b:02x}' for b in data[i:i+16])
            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
            print(f"  {i:08x}  {hex_str:<48s}  {ascii_str}")

    # Check for SLB2 magic at offset 0x4000
    with open(output, 'rb') as f:
        f.seek(0x4000)
        data = f.read(16)
        print(f"\nAt offset 0x4000 (EMC firmware):")
        hex_str = ' '.join(f'{b:02x}' for b in data)
        print(f"  {hex_str}")
        if data[:4] == b'\x53\x4c\x42\x32':
            print("  SLB2 magic found!")
        elif all(b == 0xff for b in data):
            print("  All 0xFF (empty flash region)")
        else:
            print(f"  Unknown data")

if __name__ == '__main__':
    output = sys.argv[1] if len(sys.argv) > 1 else "sflash_bar2.bin"

    # Try /dev/ps5_bar2 first, then debugfs
    for source in ['/dev/ps5_bar2', '/sys/kernel/debug/ps5_bar2_dump']:
        if os.path.exists(source):
            dump(source, output)
            sys.exit(0)

    print("Error: neither /dev/ps5_bar2 nor /sys/kernel/debug/ps5_bar2_dump found")
    print("Build and load the ps5_bar2_dump module first:")
    print("  make -f Makefile.bar2")
    print("  sudo insmod ps5_bar2_dump.ko")
    sys.exit(1)
