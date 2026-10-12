import sys,struct,os,zlib,lzma,collections
p=sys.argv[1]; out=sys.argv[2] if len(sys.argv)>2 else None
d=open(p,'rb').read()
assert d[:4]==b'DTRZ'
nf,nd,ver=struct.unpack_from('<HHB',d,4); o=9
def cstr(o):
    e=d.index(b'\0',o); return d[o:e].decode('latin1'),e+1
files=[]; 
for i in range(nf): s,o=cstr(o); files.append(s)
dirs=['']
for i in range(nd-1): s,o=cstr(o); dirs.append(s)
fmap=[]
for i in range(nf):
    di=struct.unpack_from('<H',d,o)[0]; o+=2; ch=[]
    while True:
        c=struct.unpack_from('<H',d,o)[0]; o+=2
        if c==0xffff: break
        ch.append(c)
    fmap.append((di,ch))
na,nc=struct.unpack_from('<HH',d,o); o+=4
chunks=[]
for i in range(nc):
    chunks.append(struct.unpack_from('<IIIHH',d,o)); o+=16
print('files',nf,'dirs',nd,'ver',ver,'archives',na,'chunks',nc,'hdr end',hex(o),'size',len(d))
fl=collections.Counter(c[3] for c in chunks); print('chunk flags',{hex(k):v for k,v in fl.items()})
tot=0
for (di,ch),name in zip(fmap,files):
    path=(dirs[di]+'/' if dirs[di] else '')+name
    sz=sum(chunks[c][2] for c in ch); tot+=sz
    print(f'{sz:10d} {hex(chunks[ch[0]][3]) if ch else "-":>6} {path}')
    if out:
        data=b''
        for c in ch:
            off,cl,dl,flg,af=chunks[c]
            raw=d[off:off+cl] if cl else d[off:off+dl]
            if flg&0x100 or flg&0x1==0 and flg==0: data+=d[off:off+dl]
            elif flg&0x8: data+=zlib.decompress(raw,-15 if raw[:1]!=b'\x78' else 15)
            elif flg&0x200: data+=lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(raw)
            elif flg&0x80: data+=bytes(dl)
            else: raise SystemExit(f'unhandled flag {hex(flg)} for {path} first bytes {raw[:16].hex()}')
        fp=os.path.join(out,path.replace('\\','/')); os.makedirs(os.path.dirname(fp),exist_ok=True); open(fp,'wb').write(data)
print('total uncompressed',tot)
