import os
import sys,struct,re
d=open(os.environ.get('S3E_BIN','work/biohazard.bin'),'rb').read(); img=d[0x24d0c:0x24d0c+0x170864]
for pat in sys.argv[1:]:
    for m in re.finditer(re.escape(pat.encode()),img):
        a=m.start()
        # walk back to string start
        s=a
        while s>0 and img[s-1]!=0: s-=1
        full=img[s:img.index(b'\0',s)].decode('latin1')
        refs=[]
        for cand in (s,a):
            w=struct.pack('<I',0x4a000000+cand)
            refs+=[hex(x.start()) for x in re.finditer(re.escape(w),img) if x.start()%4==0]
        print(f'{pat!r} @ {a:#x} (string start {s:#x} {full[:50]!r}) literal refs at: {sorted(set(refs))}')
