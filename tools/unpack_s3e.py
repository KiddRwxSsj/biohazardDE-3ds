#!/usr/bin/env python3
# biohazard.s3e is a raw LZMA-alone stream wrapping the XE3U image
import sys, lzma

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, 'rb').read()
out = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(raw)
assert out[:4] == b'XE3U', out[:4]
open(dst, 'wb').write(out)
print(len(raw), '->', len(out))
