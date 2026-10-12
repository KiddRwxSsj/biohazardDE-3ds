#!/usr/bin/env python3
# vtable.py ClassName [n]: find the vtable of a class through its RTTI name and list n entries
import os, sys, struct, re
d = open(os.environ.get('S3E_BIN', 'work/biohazard.bin'), 'rb').read()
img = d[0x24d0c:0x24d0c + 0x170864]
cls = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 24
mangled = f'{len(cls)}{cls}\0'.encode()
for m in re.finditer(re.escape(mangled), img):
    if m.start() and img[m.start() - 1] != 0 and not chr(img[m.start() - 1]).isalpha():
        pass
    name = m.start()
    for ti in [x.start() - 4 for x in re.finditer(re.escape(struct.pack('<I', 0x4a000000 + name)), img)]:
        for vt in [x.start() + 4 for x in re.finditer(re.escape(struct.pack('<I', 0x4a000000 + ti)), img)]:
            print(f'{cls}: typeinfo {ti:#x} vtable {vt:#x}')
            for i in range(n):
                v = struct.unpack_from('<I', img, vt + 4 * i)[0]
                print(f'  [{i:2}] +{i*4:#04x} {v - 0x4a000000 if v >= 0x4a000000 else v:#x}')
