#!/usr/bin/env python3
"""
PS5 ICC raw query tool.

Uses /dev/ps5_icc_raw (from ps5_bar2_dump.ko) to send raw ICC queries.

Usage:
  python3 ps5_icc_tool.py query <service_id> <msg_type> [data_hex]
  python3 ps5_icc_tool.py nvs-read <partition> <offset> <length>
  python3 ps5_icc_tool.py scan
  python3 ps5_icc_tool.py emc-version
"""

import ctypes
import fcntl
import os
import struct
import sys

# ioctl: _IOWR('I', 2, struct icc_raw_query) where struct is 16 bytes
ICC_IOC_MAGIC = ord('I')
ICC_RAW_QUERY = (3 << 30) | (ICC_IOC_MAGIC << 8) | 2 | (16 << 16)

ICC_MSG_MIN_SIZE = 0x20
ICC_MSG_MAX_SIZE = 0x7f0

def open_dev():
    return os.open("/dev/ps5_icc_raw", os.O_RDWR)

def icc_query(fd, query_data, max_reply=ICC_MSG_MAX_SIZE):
    buf = ctypes.create_string_buffer(query_data, max(max_reply, len(query_data)))
    rq = struct.pack('<IIQ', len(query_data), max_reply, ctypes.addressof(buf))
    rq_buf = ctypes.create_string_buffer(rq)
    fcntl.ioctl(fd, ICC_RAW_QUERY, rq_buf, True)
    _, reply_len_r, _ = struct.unpack('<IIQ', rq_buf.raw)
    return buf.raw[:reply_len_r]

def build_msg(service_id, msg_type, data=b''):
    msg = bytearray(ICC_MSG_MIN_SIZE + len(data))
    msg[0] = 0x42  # magic
    msg[1] = service_id
    struct.pack_into('<H', msg, 2, msg_type)
    struct.pack_into('<H', msg, 4, 3)  # unk_04
    total_len = ICC_MSG_MIN_SIZE + len(data)
    struct.pack_into('<H', msg, 8, total_len)
    msg[ICC_MSG_MIN_SIZE:ICC_MSG_MIN_SIZE + len(data)] = data
    return bytes(msg)

def print_reply(reply):
    if not reply or len(reply) < 12:
        print(f"  Short reply: {reply.hex() if reply else 'empty'}")
        return
    r_magic = reply[0]
    r_service = reply[1]
    r_msg_type = struct.unpack_from('<H', reply, 2)[0]
    r_unk04 = struct.unpack_from('<H', reply, 4)[0]
    r_id = struct.unpack_from('<H', reply, 6)[0]
    r_length = struct.unpack_from('<H', reply, 8)[0]
    r_checksum = struct.unpack_from('<H', reply, 10)[0]
    print(f"  magic=0x{r_magic:02x} service=0x{r_service:02x} msg_type=0x{r_msg_type:04x} "
          f"unk04={r_unk04} id={r_id} length={r_length} checksum=0x{r_checksum:04x}")
    r_data = reply[ICC_MSG_MIN_SIZE:r_length]
    if r_data:
        print(f"  data ({len(r_data)} bytes): {r_data.hex()}")
        # Try ASCII
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in r_data[:64])
        print(f"  ascii: {ascii_str}")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    fd = open_dev()

    if cmd == "query":
        if len(sys.argv) < 4:
            print("Usage: query <service_id> <msg_type> [data_hex]")
            sys.exit(1)
        service_id = int(sys.argv[2], 0)
        msg_type = int(sys.argv[3], 0)
        data = bytes.fromhex(sys.argv[4]) if len(sys.argv) > 4 else b''
        query = build_msg(service_id, msg_type, data)
        print(f"Query: service=0x{service_id:02x} msg_type=0x{msg_type:04x} data={data.hex()}")
        try:
            reply = icc_query(fd, query)
            print(f"Reply ({len(reply)} bytes):")
            print_reply(reply)
        except Exception as e:
            print(f"Error: {e}")

    elif cmd == "nvs-read":
        if len(sys.argv) < 5:
            print("Usage: nvs-read <partition> <offset> <length>")
            sys.exit(1)
        partition = int(sys.argv[2], 0)
        offset = int(sys.argv[3], 0)
        length = int(sys.argv[4], 0)
        # NVS read: service_id=0x03, msg_type=1
        # data: [0]=0, [1]=partition, [2:4]=offset(u16), [4:6]=length(u16)
        data = bytes([0, partition]) + struct.pack('<HH', offset, length)
        query = build_msg(0x03, 1, data)
        print(f"NVS read: partition={partition} offset={offset:#x} length={length}")
        try:
            reply = icc_query(fd, query)
            print(f"Reply ({len(reply)} bytes):")
            print_reply(reply)
        except Exception as e:
            print(f"Error: {e}")

    elif cmd == "emc-version":
        # Try ICC GENERAL (0x02) msg_type=0 to get EMC version
        query = build_msg(0x02, 0)
        print("Querying EMC version (service=0x02, msg_type=0)...")
        try:
            reply = icc_query(fd, query)
            print(f"Reply ({len(reply)} bytes):")
            print_reply(reply)
        except Exception as e:
            print(f"Error: {e}")

    elif cmd == "scan":
        print("Scanning ICC services...")
        for service_id in range(0x100):
            for msg_type in [0, 1, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200]:
                query = build_msg(service_id, msg_type)
                try:
                    reply = icc_query(fd, query, max_reply=256)
                    if reply and len(reply) >= 12:
                        r_len = struct.unpack_from('<H', reply, 8)[0]
                        if r_len > 0:
                            r_data = reply[ICC_MSG_MIN_SIZE:r_len]
                            # Skip empty replies (all zeros or all ff)
                            if any(b != 0 and b != 0xff for b in r_data[:8]):
                                print(f"  service=0x{service_id:02x} msg_type=0x{msg_type:04x} "
                                      f"len={r_len} data={r_data[:32].hex()}")
                except Exception:
                    pass  # Timeout or error

    elif cmd == "scan-nvs":
        print("Scanning NVS partitions...")
        for partition in range(8):
            for bank in range(2):
                # NVS read: data[0]=0, data[1]=partition, data[2:4]=offset, data[4:6]=length
                # But the partition field might encode bank too
                data = bytes([0, partition | (bank << 7)]) + struct.pack('<HH', 0, 64)
                query = build_msg(0x03, 1, data)
                try:
                    reply = icc_query(fd, query, max_reply=256)
                    if reply and len(reply) >= 12:
                        r_len = struct.unpack_from('<H', reply, 8)[0]
                        r_data = reply[ICC_MSG_MIN_SIZE:r_len]
                        print(f"  partition={partition} bank={bank}: len={r_len} data={r_data[:32].hex()}")
                except Exception as e:
                    print(f"  partition={partition} bank={bank}: error: {e}")

    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)

    os.close(fd)

if __name__ == '__main__':
    main()
