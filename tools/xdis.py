import os
import sys,struct
from capstone import *
d=open(os.environ.get('S3E_BIN','work/biohazard.bin'),'rb').read(); img=d[0x24d0c:0x24d0c+0x170864]
# import slot map
o=0x2476c+8; n=struct.unpack_from('<I',d,o)[0]; slots={}
p=0x52; names=[]
for _ in range(227):
    e=d.index(b'\0',p); names.append(d[p:e].decode()); p=e+1
for i in range(n):
    hi,lo,s=struct.unpack_from('<HHH',d,o+4+i*6); slots[(hi<<16)|lo]=names[s]
def dis(addr,n=40,stop=True):
    th=addr&1; a=addr&~1
    md=Cs(CS_ARCH_ARM,CS_MODE_THUMB if th else CS_MODE_ARM)
    c=0
    for i in md.disasm(img[a:a+n*4],a):
        extra=''
        if 'pc' in i.op_str and i.mnemonic.startswith('ldr') and '[pc' in i.op_str:
            try:
                off=int(i.op_str.split('#')[1].rstrip(']'),0); la=((i.address+(4 if th else 8))&~3)+off
                v=struct.unpack_from('<I',img,la)[0]
                extra=f'  ; ={v:#x}'
                if 0x4a000000<=v<0x4a18c000:
                    t=v-0x4a000000
                    if t in slots: extra+=f' -> slot {slots[t]}'
                    elif t<len(img):
                        s=img[t:t+40].split(b'\0')[0]
                        if len(s)>3 and all(32<=x<127 for x in s): extra+=f' "{s.decode()}"'
            except Exception as e: pass
        print(f'  {i.address:#07x}: {i.mnemonic} {i.op_str}{extra}')
        c+=1
        if c>=n: break
        if stop and (i.mnemonic in('bx',) or (i.mnemonic=='pop' and 'pc' in i.op_str)): break
if __name__=='__main__':
    for a in sys.argv[1:]:
        p=a.split(':'); print(f'--- {p[0]}'); dis(int(p[0],16),int(p[1]) if len(p)>1 else 40, len(p)<3)
