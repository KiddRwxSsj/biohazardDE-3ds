#!/usr/bin/env python3
# slotrefs.py prefix...: Thumb/ARM pc-relative loads of import slots whose name starts with a prefix
import os, sys, struct
d = open(os.environ.get('S3E_BIN', 'work/biohazard.bin'), 'rb').read()
img = d[0x24d0c:0x24d0c + 0x170864]
fo = struct.unpack_from('<I', d, 0xc)[0]
fs = struct.unpack_from('<I', d, 0x10)[0]
names, slots = [], {}
o = fo
while o < fo + fs:
    t, sz = struct.unpack_from('<II', d, o)
    p = o + 8
    if t == 0:
        n = struct.unpack_from('<H', d, p)[0]; p += 2
        for _ in range(n):
            e = d.index(b'\0', p); names.append(d[p:e].decode()); p = e + 1
    elif t == 4:
        n = struct.unpack_from('<I', d, p)[0]
        for i in range(n):
            hi, lo, s = struct.unpack_from('<HHH', d, p + 4 + i * 6)
            slots[(hi << 16) | lo] = names[s]
    o += sz
want = {k: v for k, v in slots.items() if v.startswith(tuple(sys.argv[1:]))}
for i in range(0, 0x16c018, 2):
    h = struct.unpack_from('<H', img, i)[0]
    if (h & 0xf800) == 0x4800:
        t = ((i + 4) & ~3) + (h & 0xff) * 4
        if t in want:
            print(f'{i:#x} thumb ldr r{(h >> 8) & 7} -> {want[t]}')
for i in range(0, 0x16c018, 4):
    w = struct.unpack_from('<I', img, i)[0]
    if (w & 0x0f7f0000) == 0x051f0000:
        off = w & 0xfff
        t = i + 8 + (off if w & 0x00800000 else -off)
        if t in want:
            print(f'{i:#x} arm ldr -> {want[t]}')
