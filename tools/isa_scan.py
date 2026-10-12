import sys,struct,collections
from capstone import *
from capstone.arm import *
d=open(sys.argv[1],'rb').read()
CO=0x24d0c; CS=0x170864; BASE=0x4a000000
code=d[CO:CO+CS]
A=Cs(CS_ARCH_ARM,CS_MODE_ARM); T=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
A.detail=True; T.detail=True
seen=set(); work=[(0,False)]
# extra seeds: fixup section may hold relocations; also scan data for pointers into code region (vtables/func ptrs)
ptrs=set()
for i in range(0,len(code)-3,4):
    v=struct.unpack_from('<I',code,i)[0]
    if BASE<=v<BASE+CS: ptrs.add(v-BASE)
print('abs pointers into image (base 0x4a000000):',len(ptrs))
stats=collections.Counter(); groups=collections.Counter(); mn=collections.Counter(); bad=[]
funcs={0:False}
def run(seeds):
    work=list(seeds)
    while work:
        off,th=work.pop()
        while True:
            if (off,th) in seen or off<0 or off>=CS: break
            md=T if th else A
            ins=next(md.disasm(code[off:off+4],off),None)
            if ins is None:
                bad.append((off,th,code[off:off+4].hex())); break
            seen.add((off,th)); stats['thumb' if th else 'arm']+=1
            if th and ins.size==4 and not ins.mnemonic.startswith(('bl','blx')): stats['thumb2_32bit']+=1; mn['T2:'+ins.mnemonic]+=1
            for g in ins.groups: groups[ins.group_name(g)]+=1
            m=ins.mnemonic; mn[m.split('.')[0]]+=0
            ops=ins.operands
            isbr = ARM_GRP_JUMP in ins.groups or m.startswith(('bl','b.','bx','cb')) or m=='b'
            tgt=None
            if m.split('.')[0].rstrip('w') in ('b','bl','blx','beq','bne','bhs','blo','bmi','bpl','bvs','bvc','bhi','bls','bge','blt','bgt','ble','bleq','blne','blhs','bllo','blmi','blpl','blhi','blls','blge','bllt','blgt','blle','cbz','cbnz') or (m.startswith('b') and ops and ops[-1].type==ARM_OP_IMM and ARM_GRP_JUMP in ins.groups) or m.startswith('bl'):
                if ops and ops[-1].type==ARM_OP_IMM:
                    tgt=ops[-1].imm & 0xffffffff
                    nth = (not th) if m.startswith('blx') else th
                    work.append((tgt,nth))
                    if m.startswith('bl'): funcs[tgt]=nth
            # terminators
            uncond = ins.cc in (ARM_CC_AL, ARM_CC_INVALID)
            if uncond and (m in ('b','bx','b.w','b.n') or (m in ('pop','ldm','ldmia','ldr','mov','add') and any(o.type==ARM_OP_REG and o.reg==ARM_REG_PC for o in ops[:1] if m in('ldr','mov','add')) ) or (m in('pop','ldm') and any(o.type==ARM_OP_REG and o.reg==ARM_REG_PC for o in ops))):
                break
            off+=ins.size
run([(0,False)])
print('pass1 (from entry): insns',dict(stats),'funcs',len(funcs))
# pass2: seed with pointer targets (bit0 => thumb)
seeds=[]
for p in ptrs:
    th=bool(p&1); o=p&~1
    if th or o%4==0: seeds.append((o,th))
before=dict(stats)
run(seeds)
print('pass2 (+pointer seeds, noisy): insns',dict(stats))
print('thumb function targets:',sum(1 for v in funcs.values() if v),'arm function targets:',sum(1 for v in funcs.values() if not v))
print('groups:',dict(groups))
print('thumb2 mnemonics:',{k:v for k,v in mn.items() if k.startswith('T2:')})
print('decode failures:',len(bad),bad[:5])
