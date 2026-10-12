#!/usr/bin/env python3
# XE3U (.s3e) loader + s3e/GLES host runtime on unicorn (ARM11 MPCore model)
# usage: emu.py <decompressed.s3e> <assets_dir> <outdir> [seconds]
import sys, os, struct, time, types, collections, lzma, re
from unicorn import *
from unicorn.arm_const import *

BIN, ASSETS, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
BUDGET = float(sys.argv[4]) if len(sys.argv) > 4 else 60
VERBOSE = int(os.environ.get('V', '1')); TRACEALL = int(os.environ.get('T', '0')); CFGLOG = int(os.environ.get('CFGLOG', '0')); MEMLOG = int(os.environ.get('MEMLOG', '0'))
os.makedirs(OUT, exist_ok=True)
RAM = os.path.join(OUT, 'ram'); os.makedirs(RAM, exist_ok=True)

d = open(BIN, 'rb').read()
assert d[:4] == b'XE3U'
(ver, flags, arch, fixOff, fixSize, codeOff, codeFileSize, codeMemSize, sigOff, sigSize,
 entryOff, cfgOff, cfgSize, baseOrig, extraOff, extraSize) = struct.unpack_from('<IHHIIIIIIIIIIIII', d, 4)
print(f'[ldr] XE3U ver={ver:#x} arch={arch} code={codeFileSize:#x}/{codeMemSize:#x} base={baseOrig:#x} entry={entryOff:#x}')

# fixups
syms = []; intrel = []; extrel = []
o = fixOff
while o < fixOff + fixSize:
    t, sz = struct.unpack_from('<II', d, o); p = o + 8
    if t == 0:
        n = struct.unpack_from('<H', d, p)[0]; p += 2
        for _ in range(n):
            e = d.index(b'\0', p); syms.append(d[p:e].decode()); p = e + 1
    elif t == 1:
        n = struct.unpack_from('<I', d, p)[0]; intrel = struct.unpack_from(f'<{n}I', d, p + 4)
    elif t == 4:
        n = struct.unpack_from('<I', d, p)[0]
        for i in range(n):
            hi, lo, s = struct.unpack_from('<HHH', d, p + 4 + i * 6); extrel.append(((hi << 16) | lo, s))
    o += sz
print(f'[ldr] {len(syms)} imports, {len(intrel)} internal relocs, {len(extrel)} import slots')

BASE = 0x30000000            # deliberately != original base, so relocations are really exercised
STUB = 0x60000000; RET = 0x60008000; EXIT = 0x60009000
HEAP = 0x10000000; HEAP_SIZE = 0x0A000000
STACK_TOP = 0x7ff00000; STACK_SIZE = 0x200000

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
try:
    uc.ctl_set_cpu_model(UC_CPU_ARM_11MPCORE)   # the CPU core family used by the 3DS
    print('[ldr] CPU model: ARM11 MPCore (ARMv6K)')
except Exception as e:
    print('[ldr] could not set CPU model', e)
pg = lambda x: (x + 0xfff) & ~0xfff
uc.mem_map(BASE, pg(codeMemSize) + 0x1000)
img = bytearray(d[codeOff:codeOff + codeFileSize])
delta = (BASE - baseOrig) & 0xffffffff
for r in intrel:
    v = struct.unpack_from('<I', img, r)[0]; struct.pack_into('<I', img, r, (v + delta) & 0xffffffff)
for off, s in extrel:
    if off + 4 <= len(img): struct.pack_into('<I', img, off, STUB + s * 4)
for pt in [x for x in os.environ.get('PATCH', '').split(',') if x]:
    po, pb = pt.split(':'); po = int(po, 16); pb = bytes.fromhex(pb)
    print(f'[ldr] PATCH image+{po:#x}: {img[po:po + len(pb)].hex()} -> {pb.hex()}'); img[po:po + len(pb)] = pb
uc.mem_write(BASE, bytes(img))
for off, s in extrel:
    if off + 4 > len(img): uc.mem_write(BASE + off, struct.pack('<I', STUB + s * 4))
uc.mem_map(STUB, 0x10000)
BX_R12 = struct.pack('<I', 0xe12fff1c)
uc.mem_write(STUB, BX_R12 * (0x10000 // 4))
uc.mem_map(HEAP, HEAP_SIZE)
uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE)

# helpers
R = lambda r: uc.reg_read(r)
def rd(a, n): return bytes(uc.mem_read(a, n))
def wr(a, b): uc.mem_write(a, bytes(b))
def r32(a): return struct.unpack('<I', rd(a, 4))[0]
def w32(a, v): wr(a, struct.pack('<I', v & 0xffffffff))
def cstr(a, mx=512):
    if not a: return None
    try:
        b = rd(a, mx)
    except UcError:
        return None
    return b.split(b'\0')[0].decode('latin1')
def sarg(i): return r32(R(UC_ARM_REG_SP) + 4 * i)
def s32(v): return v - (1 << 32) if v & 0x80000000 else v

class Heap:
    def __init__(s): s.top = HEAP + 0x1000; s.sz = {}; s.own = {}; s.free = collections.defaultdict(list); s.live = 0; s.peak = 0; s.by = collections.Counter(); s.peakby = collections.Counter()
    def alloc(s, n, hid=0):
        n = max(16, (n + 15) & ~15)
        if s.free[n]: a = s.free[n].pop()
        else:
            a = s.top; s.top += n
            if s.top > HEAP + HEAP_SIZE: raise MemoryError('guest heap exhausted')
        s.sz[a] = n; s.own[a] = hid; s.live += n; s.by[hid] += n; s.peak = max(s.peak, s.live); s.peakby[hid] = max(s.peakby[hid], s.by[hid]); return a
    def release(s, a):
        n = s.sz.pop(a, None)
        if n: s.free[n].append(a); s.live -= n; s.by[s.own.pop(a, 0)] -= n
    def realloc(s, a, n, hid=0):
        if not a: return s.alloc(n, hid)
        old = s.sz.get(a, 0)
        if n <= old: return a
        b = s.alloc(n, s.own.get(a, hid)); wr(b, rd(a, old)); s.release(a); return b
heap = Heap()
_strs = {}
def gstr(s):
    if s not in _strs:
        a = heap.alloc(len(s) + 1, HOST); wr(a, s.encode('latin1') + b'\0'); _strs[s] = a
    return _strs[s]

# ICF config (s3e.icf + app.icf) evaluated for OS=ANDROID
cfg = {}
def load_icf(path, osname='ANDROID'):
    active = True; grp = None
    for line in open(path, encoding='latin1'):
        line = re.split(r'\s+#|\s+//|^#|^//', line.rstrip('\r\n'))[0].strip() if not line.lstrip().startswith('{') else line.strip()
        if not line: continue
        if line.startswith('{'):
            c = line[1:line.index('}')].strip()
            if c == '': active = True
            elif c.startswith('OS='): active = osname in [x.strip() for x in c[3:].split(',')]
            elif c.startswith('['):
                m = re.match(r'\[(\w+)\]\s*(\w+)\s*==?\s*"?([^"]*)"?', c)
                active = bool(m) and str(cfg.get((m.group(1).upper(), m.group(2).upper()), '')) == m.group(3)
            else: active = False
            continue
        if line.startswith('['): grp = line[1:line.index(']')].upper(); continue
        if active and '=' in line and grp:
            k, v = line.split('=', 1); cfg[(grp, k.strip().upper())] = v.strip().strip('"')
load_icf(os.path.join(ASSETS, 's3e.icf')); load_icf(os.path.join(ASSETS, 'app.icf'))
for k, v in [x.split('=', 1) for x in os.environ.get('ICF', '').split(';') if x]:
    g, n = k.split('.'); cfg[(g.upper(), n.upper())] = v
print('[cfg] Rasterisation=%s MemSize=%s Language=%s' % (cfg.get(('GX', 'RASTERISATION')), cfg.get(('S3E', 'MEMSIZE')), cfg.get(('BIOHAZARD', 'LANGUAGE'))))

# runtime state
stats = collections.Counter(); log = []; unk = collections.Counter()
state = types.SimpleNamespace(curheap=0, ms=1000, frames=0, files={}, nextfh=0x100, userfs=[], tex={}, texn=0, curtex=0, quit=False,
                              draws=0, opened=[], trace=[], decomp={}, swaplog=[], verts=0)
W, H = 480, 320

class Call:
    def __init__(s, addr, *args): s.addr = addr; s.args = args

def resolve(name):
    name = name.replace('\\', '/')
    for pre in ('rom://', 'ram://', 'raw://', 'tmp://'):
        if name.startswith(pre): name = name[len(pre):]
    name = name.lstrip('/')
    for root in (RAM, ASSETS):
        p = os.path.join(root, name)
        if os.path.isfile(p): return p
    return None

H_ = {}
def h(fn): H_[fn.__name__] = fn; return fn

# memory
HOST = -1   # allocations made by this runtime itself, not by the game
def heap_size(hid):
    v = int(cfg.get(('S3E', 'MEMSIZE' if hid == 0 else f'MEMSIZE{hid}'), '0'))
    return v if v > 0 else 17000000 if hid == 0 else 16000000
@h
def s3eMallocBase(n):
    if heap.by[state.curheap] + n > heap_size(state.curheap):
        stats['malloc_refused'] += 1; return 0
    a = heap.alloc(n, state.curheap)
    if MEMLOG and n >= MEMLOG: print(f'   [mem] malloc {n} heap {state.curheap} -> {heap.by[state.curheap]/1e6:.2f} MB lr=image+{R(UC_ARM_REG_LR) - BASE:#x}')
    return a
@h
def s3eReallocBase(p, n):
    if MEMLOG and n >= MEMLOG: print(f'   [mem] realloc {p:#x} {n} heap {state.curheap}')
    hid = heap.own.get(p, state.curheap) if p else state.curheap
    if n > heap.sz.get(p, 0) and heap.by[hid] + n - heap.sz.get(p, 0) > heap_size(hid):
        print(f'   [mem] realloc refused p={p:#x} n={n} heap {hid} used {heap.by[hid]} limit {heap_size(hid)}')
        stats['malloc_refused'] += 1; return 0
    return heap.realloc(p, n, state.curheap)
@h
def s3eFreeBase(p):
    n = heap.sz.get(p, 0)
    heap.release(p)
    if MEMLOG and n >= MEMLOG: print(f'   [mem] free   {n} -> live {heap.live/1e6:.2f} MB')
    return 0
@h
def s3eMemorySetInt(p, v):
    if p == 0: state.curheap = v
    return 0
@h
def s3eMemoryHeapCreate(a, b, c, d):
    print(f'   [mem] s3eMemoryHeapCreate({a:#x}, {b:#x}, {c:#x}, {d:#x})'); return 0
@h
def s3eMemoryGetInt(p):
    hid = state.curheap; size = heap_size(hid); used = heap.by[hid]
    return {0: hid, 1: used, 2: size, 3: max(0, size - used), 4: max(0, size - used)}.get(p, 0)
# config
@h
def s3eConfigGetInt(g, n, out):
    v = cfg.get((cstr(g).upper(), cstr(n).upper()))
    if CFGLOG: print(f'   [cfg?] int [{cstr(g)}] {cstr(n)} -> {v}')
    if v is None: return 1
    try: w32(out, int(v, 0))
    except ValueError: return 1
    return 0
@h
def s3eConfigGetString(g, n, out):
    v = cfg.get((cstr(g).upper(), cstr(n).upper()))
    if CFGLOG: print(f'   [cfg?] str [{cstr(g)}] {cstr(n)} -> {v}')
    if v is None: return 1
    wr(out, v.encode('latin1') + b'\0'); return 0
# debug
def _trace(a, *r):
    s = cstr(a, 1024); state.trace.append(s)
    if VERBOSE: print('   [game] ' + (s or '').rstrip())
    return 0
for n_ in ('s3eDebugTraceLine', 's3eDebugOutputString', 's3eDebugPrint'): H_[n_] = _trace
@h
def s3eDebugErrorShow(t, msg): print('   [game ERROR]', cstr(msg, 1024)); state.trace.append('ERROR ' + str(cstr(msg, 1024))); return 0
@h
def s3eDebugAssertShow(t, msg): print('   [game ASSERT]', cstr(msg, 1024)); return 0
# device / timer
@h
def s3eDeviceGetInt(p):
    if CFGLOG: print(f'   [dev?] s3eDeviceGetInt({p}) lr={R(UC_ARM_REG_LR) - BASE:#x}')
    # 0 OS, 1 CLASS, 2 ID, 3 CAPS? ... language = 9 (S3E_DEVICE_LANGUAGE); values per s3eDevice.h: ENGLISH=1 ... SPANISH=7
    return {0: 12, 9: int(os.environ.get('LANG_ID', '1'))}.get(p, 0)
@h
def s3eDeviceGetString(p): return gstr('emu')
@h
def s3eDeviceCheckQuitRequest(): return 1 if state.quit else 0
@h
def s3eDeviceYield(ms=0): state.ms += max(1, min(s32(ms), 50)); return 0
@h
def s3eDeviceYieldUntilEvent(ms=0): state.ms += max(1, min(s32(ms), 50)); return 0
def _ms(*a):
    state.ms += 1; uc.reg_write(UC_ARM_REG_R1, 0); return state.ms & 0xffffffff
H_['s3eTimerGetMs'] = _ms
@h
def s3eTimerGetUTC(): uc.reg_write(UC_ARM_REG_R1, 0x130); return 0x5a000000
@h
def s3eTimerGetInt(p): return 1
# surface
@h
def s3eSurfaceGetInt(p): return {0: W, 1: H, 2: W * 2, 3: 0x125, 4: W, 5: H, 6: 0x125, 7: W * 2}.get(p, 0)
@h
def s3eGLGetInt(p): return {0: 0x110}.get(p, 1)
@h
def s3eGLGetNativeWindow(): return 0x1234
# files
class FH:
    def __init__(s, data, name, mode='rb', path=None): s.data = bytearray(data); s.pos = 0; s.name = name; s.mode = mode; s.path = path
class UFH:   # file living in a guest-registered user filesystem (Derbh archive)
    def __init__(s, fs, h, name): s.fs = fs; s.h = h; s.name = name
SCR = None
def scratch():
    global SCR
    if SCR is None: SCR = heap.alloc(64, HOST)
    return SCR
def ufs_open(name_ptr, mode_ptr):
    for fs in reversed(state.userfs):
        hnd = yield Call(r32(fs + 0), name_ptr, mode_ptr)
        if hnd: return UFH(fs, hnd, cstr(name_ptr))
    return None
@h
def s3eFileOpen(name, mode):
    n = cstr(name); m = cstr(mode) or 'rb'
    if 'r' in m:
        u = yield from ufs_open(name, mode)
        if u:
            state.opened.append((n, m, 'dz'))
            if VERBOSE: print(f'   [file] open {n!r} {m!r} -> ok (inside main.dz via game Derbh callbacks)')
            fh = state.nextfh; state.nextfh += 4; state.files[fh] = u; return fh
    p = resolve(n)
    state.opened.append((n, m, bool(p)))
    if VERBOSE: print(f'   [file] open {n!r} {m!r} -> {"ok" if p or "w" in m else "NOT FOUND"}')
    if 'w' in m or ('a' in m and not p):
        path = os.path.join(RAM, n.replace('\\', '/').split('://')[-1].lstrip('/')); os.makedirs(os.path.dirname(path), exist_ok=True)
        f = FH(b'', n, m, path)
    elif p: f = FH(open(p, 'rb').read(), n, m, p if '+' in m or 'a' in m else None)
    else: return 0
    fh = state.nextfh; state.nextfh += 4; state.files[fh] = f; return fh
@h
def s3eFileOpenFromMemory(buf, size):
    f = FH(rd(buf, size), '<mem>'); fh = state.nextfh; state.nextfh += 4; state.files[fh] = f; return fh
@h
def s3eFileClose(fh):
    f = state.files.pop(fh, None)
    if isinstance(f, UFH): yield Call(r32(f.fs + 20), f.h); return 0
    if f and f.path and ('w' in f.mode or 'a' in f.mode or '+' in f.mode): open(f.path, 'wb').write(f.data)
    return 0
@h
def s3eFileRead(buf, es, n, fh):
    f = state.files.get(fh)
    if isinstance(f, UFH):
        r = yield Call(r32(f.fs + 4), buf, es, n, f.h); stats['bytes_read_dz'] += r * es; return r
    if not f or not es: return 0
    cnt = min(n, (len(f.data) - f.pos) // es); b = f.data[f.pos:f.pos + cnt * es]; f.pos += len(b)
    if b: wr(buf, b)
    stats['bytes_read'] += len(b); return cnt
@h
def s3eFileWrite(buf, es, n, fh):
    f = state.files.get(fh)
    if not f or isinstance(f, UFH): return 0
    b = rd(buf, es * n); f.data[f.pos:f.pos + len(b)] = b; f.pos += len(b); return n
@h
def s3eFileSeek(fh, off, origin):
    f = state.files.get(fh)
    if isinstance(f, UFH):
        r = yield Call(r32(f.fs + 12), f.h, off, origin); return r
    if not f: return 1
    off = s32(off); f.pos = max(0, {0: off, 1: f.pos + off, 2: len(f.data) + off}.get(origin, off)); return 0
@h
def s3eFileTell(fh):
    f = state.files.get(fh)
    if isinstance(f, UFH):
        r = yield Call(r32(f.fs + 16), f.h); return r
    return f.pos if f else 0xffffffff
@h
def s3eFileGetSize(fh):
    f = state.files.get(fh)
    if isinstance(f, UFH):
        cur = yield Call(r32(f.fs + 16), f.h)
        yield Call(r32(f.fs + 12), f.h, 0, 2)
        size = yield Call(r32(f.fs + 16), f.h)
        yield Call(r32(f.fs + 12), f.h, cur, 0)
        return size
    return len(f.data) if f else 0xffffffff
@h
def s3eFileGetChar(fh):
    f = state.files.get(fh)
    if isinstance(f, UFH):
        b = scratch(); r = yield Call(r32(f.fs + 4), b, 1, 1, f.h)
        return rd(b, 1)[0] if r == 1 else 0xffffffff
    if not f or f.pos >= len(f.data): return 0xffffffff
    f.pos += 1; return f.data[f.pos - 1]
@h
def s3eFileCheckExists(name):
    n = cstr(name)
    u = yield from ufs_open(name, gstr('rb'))
    if u:
        yield Call(r32(u.fs + 20), u.h); return 1
    return 1 if resolve(n) else 0
@h
def s3eFileGetError(): return 0
@h
def s3eFileFlush(fh): return 0
@h
def s3eFileMakeDirectory(n): return 0
@h
def s3eFileAddUserFileSys(p):
    c = heap.alloc(64, HOST); wr(c, rd(p, 64)); state.userfs.append(c); return 0
@h
def s3eExtGet(name, buf, size):
    if VERBOSE: print('   [ext] s3eExtGet', cstr(name));
    return 1
@h
def s3eSecureStorageGet(buf, size):
    p = os.path.join(RAM, 'secure.bin')
    if not os.path.isfile(p): return 1
    wr(buf, open(p, 'rb').read()[:size]); return 0
@h
def s3eSecureStoragePut(buf, size): open(os.path.join(RAM, 'secure.bin'), 'wb').write(rd(buf, size)); return 0
@h
def s3eSecureStorageGetError(): return 0
# compression (host side, like the real loader does)
class Dec:
    def __init__(s, algo, cb, user): s.algo = algo; s.cb = cb; s.user = user; s.z = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE); s.out = bytearray(); s.req = heap.alloc(16, HOST); s.buf = heap.alloc(0x4000, HOST)
@h
def s3eCompressionDecompInit(algo, cb, user):
    hnd = state.nextfh; state.nextfh += 4; state.decomp[hnd] = Dec(algo, cb, user); stats['decomp_streams'] += 1
    return hnd
@h
def s3eCompressionDecompRead(hnd, buf, plen):
    dc = state.decomp[hnd]; n = r32(plen)
    while len(dc.out) < n and not dc.z.eof:
        w32(dc.req, dc.buf); w32(dc.req + 4, 0x4000)
        got = yield Call(dc.cb, dc.req, dc.user)
        if not got or got > 0x4000: break
        dc.out += dc.z.decompress(rd(dc.buf, got))
    chunk = bytes(dc.out[:n]); del dc.out[:n]
    if chunk: wr(buf, chunk)
    w32(plen, len(chunk)); stats['bytes_decompressed'] += len(chunk)
    return 0
@h
def s3eCompressionDecompFinal(hnd):
    dc = state.decomp.pop(hnd, None)
    if dc: heap.release(dc.req); heap.release(dc.buf)
    return 0
@h
def s3eCompressionDecomp(inbuf, inlen, pout, poutlen, algo):
    raise NotImplementedError('s3eCompressionDecomp one-shot')
# input: keyboard/pointer
@h
def s3eKeyboardGetState(k): return 0
@h
def s3eKeyboardAnyKey(): return 0
# touch input script: TAPS="frame:x:y,frame:x:y" (press at frame, release 4 frames later)
TAPS = [tuple(int(v) for v in t.split(':')) for t in os.environ.get('TAPS', '').split(',') if t]
ptr = types.SimpleNamespace(cbs={}, x=0, y=0, st=0, ev=None)
@h
def s3ePointerRegister(ev, cb, user): ptr.cbs[ev] = (cb, user); return 0
@h
def s3ePointerUpdate():
    f = state.frames; ptr.st = {2: 1, 4: 0}.get(ptr.st, ptr.st)
    for (tf, x, y) in TAPS:
        for when, pressed in ((tf, 1), (tf + 4, 0)):
            if f == when and ptr.ev != (when, pressed):
                ptr.ev = (when, pressed); ptr.x, ptr.y = x, y; ptr.st = 2 if pressed else 4
                print(f'   [input] touch {"down" if pressed else "up"} at ({x},{y}) frame {f}')
                e = scratch() + 16
                if 2 in ptr.cbs:      # S3E_POINTER_TOUCH_EVENT {id, pressed, x, y}
                    wr(e, struct.pack('<IIii', 0, pressed, x, y)); yield Call(ptr.cbs[2][0], e, ptr.cbs[2][1])
                if 0 in ptr.cbs:      # S3E_POINTER_BUTTON_EVENT {button, pressed, x, y}
                    wr(e, struct.pack('<IIii', 0, pressed, x, y)); yield Call(ptr.cbs[0][0], e, ptr.cbs[0][1])
    return 0
@h
def s3ePointerGetState(b): return ptr.st if b == 0 else 0
@h
def s3ePointerGetX(): return ptr.x
@h
def s3ePointerGetY(): return ptr.y
@h
def s3ePointerGetInt(p): return 1
# sound/audio/video
@h
def s3eSoundGetInt(p): return {0: 100, 1: 22050, 2: 8, 3: 0, 4: 0}.get(p, 0)
@h
def s3eSoundGetFreeChannel(): return 0
@h
def s3eSoundChannelGetInt(c, p): return 0
@h
def s3eAudioIsPlaying(): return 0
@h
def s3eAudioGetInt(p): return 0
@h
def s3eVideoIsPlaying(): return 0
@h
def s3eVideoPlay(name, rep, x, y, w=0):
    print('   [video] s3eVideoPlay', cstr(name)); state.trace.append('VIDEO ' + str(cstr(name))); return 0
@h
def s3eAccelerometerGetInt(p): return 0
@h
def s3eVibraGetInt(p): return 0
@h
def s3eDebugGetInt(p): return 0
@h
def s3eDebugIsDebuggerPresent(): return 0
@h
def s3eDeviceExit(code=0): print('   [game] s3eDeviceExit', code); state.quit = True; uc.emu_stop(); return 0

# EGL / GL ES 1.x (state tracking only + texture capture)
@h
def eglGetDisplay(d): return 1
@h
def eglInitialize(d, maj, mnr):
    if maj: w32(maj, 1)
    if mnr: w32(mnr, 1)
    return 1
@h
def eglChooseConfig(d, attr, cfgs, size, num):
    if cfgs and size: w32(cfgs, 1)
    if num: w32(num, 1)
    return 1
@h
def eglGetConfigs(d, cfgs, size, num):
    if cfgs and size: w32(cfgs, 1)
    if num: w32(num, 1)
    return 1
@h
def eglGetConfigAttrib(d, c, a, v):
    w32(v, {0x3020: 16, 0x3021: 0, 0x3022: 5, 0x3023: 6, 0x3024: 5, 0x3025: 16, 0x3026: 0, 0x3028: 1, 0x3033: 4, 0x3040: 1}.get(a, 0)); return 1
@h
def eglCreateWindowSurface(d, c, w, a): return 0x51
@h
def eglCreateContext(d, c, s, a): return 0xC1
@h
def eglMakeCurrent(d, dr, rd_, c): return 1
@h
def eglQuerySurface(d, s, a, v): w32(v, {0x3057: W, 0x3056: H}.get(a, 0)); return 1
@h
def eglGetError(): return 0x3000
@h
def eglQueryString(d, n): return gstr('1.1 emu')
@h
def eglSwapBuffers(d, s):
    state.frames += 1; state.ms += 33
    state.swaplog.append((state.draws, state.verts)); state.draws = 0; state.verts = 0
    if hasattr(state, 'on_frame'): state.on_frame()
    return 1
for n_ in ('eglSwapInterval', 'eglBindAPI', 'eglWaitGL', 'eglWaitNative', 'eglWaitClient', 'eglDestroySurface', 'eglDestroyContext', 'eglTerminate', 'eglSurfaceAttrib', 'eglReleaseThread'):
    H_[n_] = lambda *a: 1
@h
def eglGetCurrentContext(): return 0xC1
@h
def eglGetCurrentDisplay(): return 1
@h
def eglGetCurrentSurface(w): return 0x51
@h
def glGetString(n): return gstr({0x1F00: 'emu', 0x1F01: 'emu GLES1 stub', 0x1F02: 'OpenGL ES-CM 1.1', 0x1F03: ''}.get(n, ''))
@h
def glGetIntegerv(p, out): w32(out, {0x0D33: 1024, 0x84E2: 2, 0x0D31: 8, 0x0D3A: 2048, 0x86A2: 0}.get(p, 0)); return 0
@h
def glGetBooleanv(p, out): wr(out, b'\0'); return 0
@h
def glGenTextures(n, out):
    for i in range(n): state.texn += 1; w32(out + 4 * i, state.texn)
    return 0
@h
def glGenBuffers(n, out):
    for i in range(n): state.texn += 1; w32(out + 4 * i, state.texn)
    return 0
@h
def glBindTexture(t, id_): state.curtex = id_; return 0
def _tex(target, level, ifmt, w):
    h_, border, fmt, typ, pix = sarg(0), sarg(1), sarg(2), sarg(3), sarg(4)
    stats['tex_uploads'] += 1
    if level == 0:
        bpp = {(0x1908, 0x1401): 4, (0x1907, 0x1401): 3, (0x1909, 0x1401): 1, (0x1906, 0x1401): 1, (0x190A, 0x1401): 2}.get((fmt, typ), 2)
        data = rd(pix, w * h_ * bpp) if pix else None
        state.tex[state.curtex] = (w, h_, fmt, typ, data)
    return 0
H_['glTexImage2D'] = _tex
@h
def glCompressedTexImage2D(target, level, ifmt, w, *r): stats['tex_compressed'] += 1; state.tex[state.curtex] = (w, sarg(0), ifmt, 0, None); return 0
@h
def glDrawElements(mode, count, typ, idx): state.draws += 1; state.verts += count; stats['draw_calls'] += 1; return 0
@h
def glDrawArrays(mode, first, count): state.draws += 1; state.verts += count; stats['draw_calls'] += 1; return 0

# dispatch
ring = collections.deque(maxlen=14)
conts = []   # stack of (generator, saved_lr)
def step(gen, saved_lr, send=None):
    try:
        req = gen.send(send)
    except StopIteration as e:
        v = e.value
        if v is not None: uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)
        uc.reg_write(UC_ARM_REG_R12, saved_lr); return
    conts.append((gen, saved_lr))
    for i, a in enumerate(req.args[:4]): uc.reg_write(UC_ARM_REG_R0 + i, a & 0xffffffff)
    assert len(req.args) <= 4
    uc.reg_write(UC_ARM_REG_LR, RET); uc.reg_write(UC_ARM_REG_R12, req.addr)

def on_stub(uc_, addr, size, ud):
    lr = R(UC_ARM_REG_LR)
    if addr == RET:
        gen, saved = conts.pop(); step(gen, saved, R(UC_ARM_REG_R0)); return
    if addr == EXIT:
        print('[emu] entry point returned', R(UC_ARM_REG_R0)); state.quit = True; uc.emu_stop(); return
    name = syms[(addr - STUB) // 4]; stats[name] += 1
    ring.append((name, R(UC_ARM_REG_R0), R(UC_ARM_REG_R1), R(UC_ARM_REG_R2), R(UC_ARM_REG_R3), lr - BASE))
    if TRACEALL and stats['_t'] < TRACEALL and name not in ('s3eMallocBase','s3eFreeBase','s3eReallocBase'):
        stats['_t'] += 1; print(f'   [call] {name}({R(UC_ARM_REG_R0):#x}, {R(UC_ARM_REG_R1):#x}, {R(UC_ARM_REG_R2):#x}, {R(UC_ARM_REG_R3):#x}) lr={lr - BASE:#x}')
    fn = H_.get(name)
    if fn is None:
        unk[name] += 1
        if unk[name] == 1 and VERBOSE and not name.startswith(('gl', 'egl')):
            print(f'   [stub] {name}({R(UC_ARM_REG_R0):#x}, {R(UC_ARM_REG_R1):#x}, {R(UC_ARM_REG_R2):#x}, {R(UC_ARM_REG_R3):#x}) lr={lr - BASE:#x}')
        uc.reg_write(UC_ARM_REG_R0, 0); uc.reg_write(UC_ARM_REG_R12, lr); return
    n = fn.__code__.co_argcount if hasattr(fn, '__code__') else 4
    args = [R(UC_ARM_REG_R0 + i) for i in range(min(n, 4))] + [sarg(i - 4) for i in range(4, n)]
    if fn.__code__.co_flags & 0x04: args = [R(UC_ARM_REG_R0 + i) for i in range(4)]
    res = fn(*args)
    if isinstance(res, types.GeneratorType): step(res, lr); return
    if res is not None: uc.reg_write(UC_ARM_REG_R0, res & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, lr)
uc.hook_add(UC_HOOK_CODE, on_stub, begin=STUB, end=STUB + 0xffff)

def on_bad(uc_, access, addr, size, value, ud):
    pc = R(UC_ARM_REG_PC)
    print(f'[emu] INVALID MEMORY access={access} addr={addr:#x} size={size} pc={pc:#x} (image+{pc - BASE:#x}) lr=image+{R(UC_ARM_REG_LR) - BASE:#x}')
    print('   regs:', ' '.join(f'r{i}={R(UC_ARM_REG_R0 + i):#x}' for i in range(8)))
    sp = R(UC_ARM_REG_SP); cand = []
    for i in range(0, 0x300, 4):
        v = r32(sp + i)
        if BASE <= v < BASE + codeFileSize and (v & 1): cand.append(hex(v - BASE))
    print('   stack return-address candidates:', cand[:24])
    for c in ring: print('   last call:', c[0], [hex(x) for x in c[1:5]], 'lr=' + hex(c[5]))
    return False
uc.hook_add(UC_HOOK_MEM_INVALID, on_bad)
if os.environ.get('WATCHP'):
    _pa, _po = [int(x, 16) for x in os.environ['WATCHP'].split(':')]
    def on_wp(uc_, access, addr, size, value, ud):
        p = r32(BASE + _pa)
        if p and addr <= p + _po < addr + size:
            print(f'   [watchp] write *({_pa:#x})+{_po:#x} size={size} value={value:#x} pc=image+{R(UC_ARM_REG_PC) - BASE:#x} lr=image+{R(UC_ARM_REG_LR) - BASE:#x}')
    uc.hook_add(UC_HOOK_MEM_WRITE, on_wp, begin=HEAP, end=HEAP + HEAP_SIZE)
if os.environ.get('WATCH'):
    for wa in os.environ['WATCH'].split(','):
        wa = int(wa, 16)
        def on_w(uc_, access, addr, size, value, ud):
            print(f'   [watch] write image+{addr - BASE:#x} size={size} value={value:#x} pc=image+{R(UC_ARM_REG_PC) - BASE:#x} lr=image+{R(UC_ARM_REG_LR) - BASE:#x}')
        uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=BASE + wa, end=BASE + wa + 3)

def run(budget=BUDGET, extra=None):
    if extra: extra()
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP - 0x100); uc.reg_write(UC_ARM_REG_LR, EXIT)
    pc = BASE + entryOff; t0 = time.time(); err = None
    try:
        uc.emu_start(pc, EXIT + 4, timeout=int(budget * 1_000_000))
    except UcError as e:
        err = e
    el = time.time() - t0
    pc = R(UC_ARM_REG_PC)
    print(f'\n[emu] stopped after {el:.1f}s host time; pc=image+{pc - BASE:#x}; error={err}')
    print(f'[emu] frames presented (eglSwapBuffers): {state.frames}; guest heap peak: {heap.peak / 1e6:.1f} MB, per s3e heap: { {k: round(v / 1e6, 2) for k, v in heap.peakby.items()} }, refused mallocs: {stats['malloc_refused']}; bytes read from files: {stats["bytes_read"] / 1e6:.1f} MB')
    print(f'[emu] textures uploaded: {stats["tex_uploads"]} (+{stats["tex_compressed"]} compressed); draw calls: {stats["draw_calls"]}')
    print('[emu] unimplemented imports hit (returned 0):', dict(unk.most_common(40)))
    return err

if __name__ == '__main__':
    if os.environ.get('GLSOFT'):
        exec(compile(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'glsoft.py')).read(), 'glsoft.py', 'exec'))
    if os.environ.get('GLSURVEY'):
        exec(compile(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'glsurvey.py')).read(), 'glsurvey.py', 'exec'))
    for _x in [x for x in os.environ.get('EXTRA', '').split(',') if x]:
        exec(compile(open(_x).read(), _x, 'exec'))
    run()
    if os.environ.get('GLSOFT'): print('[gl] texture formats:', dict(gl.fmts), 'primitive modes:', dict(gl.modes))
    print('[emu] files opened:', len(state.opened))
