#!/usr/bin/env python3
"""
PS5 Serial Flash and ICC tool.

Uses new ioctls on /dev/icc to:
  - Dump the 2MB BAR2 region (serial flash window) 
  - Send raw ICC queries to the EMC firmware
  - Read ICC NVS partitions

Usage:
  python3 ps5_sflash.py dump-bar2 [output_file]
  python3 ps5_sflash.py icc-query <service_id> <msg_type> [data_hex...]
  python3 ps5_sflash.py nvs-read <partition> <offset> <length>
  python3 ps5_sflash.py scan-icc
"""

import ctypes
import fcntl
import os
import struct
import sys

# ioctl definitions
ICC_IOC_MAGIC = ord('I')

# struct icc_raw_query { u32 query_len; u32 reply_len; u64 data; }
ICC_RAW_QUERY = struct.pack('B', ICC_IOC_MAGIC)  # _IOWR('I', 2, struct icc_raw_query)
# Calculate ioctl number: _IOWR(type, nr, size)
# _IOWR = (3 << 30) | (type << 8) | nr | (size << 16)
# struct icc_raw_query size = 4+4+8 = 16 bytes
ICC_RAW_QUERY_NR = (3 << 30) | (ICC_IOC_MAGIC << 8) | 2 | (16 << 16)

# struct icc_bar2_read { u32 offset; u32 size; u64 data; }
# _IOWR('I', 3, struct icc_bar2_read) = 16 bytes
ICC_BAR2_READ_NR = (3 << 30) | (ICC_IOC_MAGIC << 8) | 3 | (16 << 16)

# ICC message structure: magic(1) + service_id(1) + msg_type(2) + unk_04(2) + id(2) + length(2) + checksum(2) = 12 bytes header
ICC_MSG_MIN_SIZE = 0x20
ICC_MSG_MAX_SIZE = 0x7f0

def open_icc():
    fd = os.open("/dev/icc", os.O_RDWR)
    return fd

def bar2_read(fd, offset, size):
    """Read from BAR2 (2MB serial flash window) via ioctl."""
    buf = ctypes.create_string_buffer(size)
    br = struct.pack('<IIQ', offset, size, ctypes.addressof(buf))
    br_buf = ctypes.create_string_buffer(br)
    fcntl.ioctl(fd, ICC_BAR2_READ_NR, br_buf, True)
    # Parse reply
    offset_r, size_r, data_ptr = struct.unpack('<IIQ', br_buf.raw)
    return buf.raw[:size_r]

def dump_bar2(fd, output_file, total_size=0x200000, chunk_size=4096):
    """Dump the entire 2MB BAR2 region."""
    print(f"Dumping BAR2 (2MB) to {output_file}...")
    with open(output_file, 'wb') as f:
        for offset in range(0, total_size, chunk_size):
            data = bar2_read(fd, offset, min(chunk_size, total_size - offset))
            f.write(data)
            if offset % (64 * 1024) == 0:
                print(f"  {offset:#08x}/{total_size:#08x} ({100*offset//total_size}%)")
    print(f"Done. {total_size} bytes written to {output_file}")

def icc_raw_query(fd, query_data, max_reply=ICC_MSG_MAX_SIZE):
    """Send raw ICC query and get reply."""
    buf = ctypes.create_string_buffer(query_data, max(max_reply, len(query_data)))
    rq = struct.pack('<IIQ', len(query_data), max_reply, ctypes.addressof(buf))
    rq_buf = ctypes.create_string_buffer(rq)
    fcntl.ioctl(fd, ICC_RAW_QUERY_NR, rq_buf, True)
    # Parse reply
    query_len_r, reply_len_r, data_ptr = struct.unpack('<IIQ', rq_buf.raw)
    return buf.raw[:reply_len_r]

def build_icc_msg(service_id, msg_type, data=b''):
    """Build an ICC message with proper header."""
    msg = bytearray(ICC_MSG_MIN_SIZE + len(data))
    msg[0] = 0x42  # magic (set by driver, but we set it anyway)
    msg[1] = service_id
    struct.pack_into('<H', msg, 2, msg_type)
    # unk_04 = 3 (set by driver)
    struct.pack_into('<H', msg, 4, 3)
    # id = 0 (set by driver)
    # length = total message length
    total_len = ICC_MSG_MIN_SIZE + len(data)
    struct.pack_into('<H', msg, 8, total_len)
    # checksum = 0 (set by driver)
    # data
    msg[ICC_MSG_MIN_SIZE:ICC_MSG_MIN_SIZE + len(data)] = data
    return bytes(msg)

def nvs_read(fd, partition, offset, length):
    """Read from ICC NVS (service_id=0x03, msg_type=1)."""
    data = struct.pack('<BHH', 0, partition, offset, length)
    query = build_icc_msg(0x03, 1, data)
    reply = icc_raw_query(fd, query)
    # Reply data starts at offset 2 in the data field
    reply_msg = reply[ICC_MSG_MIN_SIZE:]
    return reply_msg[2:2+length]

def scan_icc(fd):
    """Scan ICC services and msg_types to find which ones respond."""
    print("Scanning ICC services...")
    for service_id in range(0x100):
        for msg_type in [0, 1, 0x10, 0x20, 0x40, 0x80]:
            query = build_icc_msg(service_id, msg_type)
            try:
                reply = icc_raw_query(fd, query, max_reply=256)
                if reply and len(reply) >= 12:
                    reply_msg = struct.unpack_from('<H', reply, 2)[0]
                    reply_len = struct.unpack_from('<H', reply, 8)[0]
                    if reply_len > 0:
                        print(f"  service=0x{service_id:02x} msg_type=0x{msg_type:04x} -> reply_type=0x{reply_msg:04x} len={reply_len}")
                        # Show first few bytes of reply data
                        data = reply[ICC_MSG_MIN_SIZE:ICC_MSG_MIN_SIZE+16]
                        print(f"    data: {data.hex()}")
            except Exception as e:
                pass  # Timeout or error, skip

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    fd = open_icc()

    if cmd == "dump-bar2":
        output = sys.argv[2] if len(sys.argv) > 2 else "sflash_bar2.bin"
        dump_bar2(fd, output)

    elif cmd == "icc-query":
        if len(sys.argv) < 4:
            print("Usage: icc-query <service_id> <msg_type> [data_hex...]")
            sys.exit(1)
        service_id = int(sys.argv[2], 0)
        msg_type = int(sys.argv[3], 0)
        data = b''
        if len(sys.argv) > 4:
            data = bytes.fromhex(sys.argv[4])
        query = build_icc_msg(service_id, msg_type, data)
        print(f"Query: {query.hex()}")
        reply = icc_raw_query(fd, query)
        print(f"Reply ({len(reply)} bytes): {reply.hex()}")
        # Parse reply header
        if len(reply) >= 12:
            r_magic = reply[0]
            r_service = reply[1]
            r_msg_type = struct.unpack_from('<H', reply, 2)[0]
            r_length = struct.unpack_from('<H', reply, 8)[0]
            print(f"  magic=0x{r_magic:02x} service=0x{r_service:02x} msg_type=0x{r_msg_type:04x} length={r_length}")
            r_data = reply[ICC_MSG_MIN_SIZE:r_length]
            if r_data:
                print(f"  data: {r_data.hex()}")

    elif cmd == "nvs-read":
        if len(sys.argv) < 5:
            print("Usage: nvs-read <partition> <offset> <length>")
            sys.exit(1)
        partition = int(sys.argv[2], 0)
        offset = int(sys.argv[3], 0)
        length = int(sys.argv[4], 0)
        data = nvs_read(fd, partition, offset, length)
        print(f"NVS partition={partition} offset={offset:#x} length={length}:")
        print(f"  {data.hex()}")

    elif cmd == "scan-icc":
        scan_icc(fd)

    elif cmd == "bar2-read":
        if len(sys.argv) < 4:
            print("Usage: bar2-read <offset> <size>")
            sys.exit(1)
        offset = int(sys.argv[2], 0)
        size = int(sys.argv[3], 0)
        data = bar2_read(fd, offset, size)
        print(f"BAR2 offset={offset:#x} size={size}:")
        # Print in hex dump format
        for i in range(0, len(data), 16):
            hex_str = ' '.join(f'{b:02x}' for b in data[i:i+16])
            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
            print(f"  {offset+i:08x}  {hex_str:<48s}  {ascii_str}")

    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)

    os.close(fd)

if __name__ == '__main__':
    main()
