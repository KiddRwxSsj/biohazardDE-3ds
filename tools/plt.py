#!/usr/bin/env python3
# plt.py [prefix...]: map import thunks (add ip,pc / add ip,ip / ldr pc,[ip,#off]!) to names, list callers
import os, sys, struct, subprocess
d = open(os.environ.get('S3E_BIN', 'work/biohazard.bin'), 'rb').read()
img = d[0x24d0c:0x24d0c + 0x170864]
fo, fs = struct.unpack_from('<II', d, 0xc)
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

def rot(imm12):
    r = (imm12 >> 8) * 2; v = imm12 & 0xff
    return ((v >> r) | (v << (32 - r))) & 0xffffffff

thunks = {}
for a in range(0, 0x2000, 4):
    w0, w1, w2 = struct.unpack_from('<III', img, a)
    if (w0 & 0xfffff000) == 0xe28fc000 and (w1 & 0xfffff000) == 0xe28cc000 and (w2 & 0xfffff000) == 0xe5bcf000:
        slot = a + 8 + rot(w0 & 0xfff) + rot(w1 & 0xfff) + (w2 & 0xfff)
        if slot in slots:
            thunks[a] = slots[slot]
want = {a: n for a, n in thunks.items() if not sys.argv[1:] or n.startswith(tuple(sys.argv[1:]))}
for i in range(0, 0x16c018 - 3, 2):
    h1, h2 = struct.unpack_from('<HH', img, i)
    if (h1 & 0xf800) == 0xf000 and (h2 & 0xe800) == 0xe800:
        off = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1)
        if off & 0x400000:
            off -= 0x800000
        dst = i + 4 + off
        if h2 & 0x1000 == 0:
            dst &= ~3
        if dst in want:
            print(f'{i:#x} -> {want[dst]}')
