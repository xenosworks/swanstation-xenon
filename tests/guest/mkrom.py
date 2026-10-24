#!/usr/bin/env python3
"""mkrom.py in.bin out.bin [seed] [mode] [count]: pad a test program to a BIOS image and set its parameters."""
import struct
import sys

data = bytearray(open(sys.argv[1], 'rb').read())
if len(data) > 512 * 1024:
    sys.exit('image too large')
data += b'\0' * (512 * 1024 - len(data))
seed = int(sys.argv[3], 0) if len(sys.argv) > 3 else 1
mode = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0xFFFFFFFF
struct.pack_into('<II', data, 0x100, seed, mode)
if len(sys.argv) > 5:
    struct.pack_into('<I', data, 0x108, int(sys.argv[5], 0))
open(sys.argv[2], 'wb').write(data)
