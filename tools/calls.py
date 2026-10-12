#!/usr/bin/env python3
# calls.py <target-hex>...: find Thumb BL/BLX and ARM BL call sites to an image offset
import os, sys, struct
d = open(os.environ.get('S3E_BIN', 'work/biohazard.bin'), 'rb').read()
img = d[0x24d0c:0x24d0c + 0x16c018]
targets = [int(a, 16) & ~1 for a in sys.argv[1:]]
for i in range(0, len(img) - 3, 2):
    h1, h2 = struct.unpack_from('<HH', img, i)
    if (h1 & 0xf800) == 0xf000 and (h2 & 0xe800) == 0xe800:
        off = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1)
        if off & 0x400000:
            off -= 0x800000
        dst = i + 4 + off
        if h2 & 0x1000 == 0:
            dst &= ~3
        if dst in targets:
            print(f'{i:#x} -> {dst:#x}')
