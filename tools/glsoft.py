# software GLES 1.x subset, exec'd inside emu.py's namespace (GLSOFT=1)
import numpy as np
from PIL import Image

RENDER_FRAMES = set(int(x) for x in os.environ.get('FRAMES', '').split(',') if x)
RENDER_EVERY = int(os.environ.get('EVERY', '0'))
FX = 1.0 / 65536.0
fx = lambda v: s32(v) * FX

class GL: pass
gl = GL()
gl.mode = 0x1700
gl.mats = {0x1700: [np.eye(4)], 0x1701: [np.eye(4)], 0x1702: [np.eye(4)], 'tex1': [np.eye(4)]}
gl.viewport = (0, 0, W, H); gl.drange = (0.0, 1.0)
gl.en = set(); gl.texen = [False, False]; gl.acttex = 0; gl.cacttex = 0; gl.bound = [0, 0]
gl.arr = {}; gl.cen = set(); gl.color = np.ones(4)
gl.blend = (1, 0); gl.afunc = (0x207, 0.0); gl.dfunc = 0x201; gl.dmask = True; gl.cmask = True
gl.cull = 0x405; gl.front = 0x901; gl.clear = np.zeros(4)
gl.bufs = {}; gl.abuf = 0; gl.ebuf = 0
gl.texdata = {}; gl.texenv = [0x2100, 0x2100]; gl.texparam = {}
gl.fb = np.zeros((H, W, 4), np.float32); gl.zb = np.ones((H, W), np.float32)
gl.rendering = False; gl.fmts = collections.Counter(); gl.modes = collections.Counter(); gl.tris = 0
gl.scissor = None

def cur():
    k = gl.mode
    if k == 0x1702 and gl.acttex == 1: k = 'tex1'
    return gl.mats[k]
def mul(m): c = cur(); c[-1] = c[-1] @ m

@h
def glMatrixMode(m): gl.mode = m
@h
def glLoadIdentity(): cur()[-1] = np.eye(4)
@h
def glLoadMatrixx(p): cur()[-1] = (np.frombuffer(rd(p, 64), '<i4').astype(np.float64) * FX).reshape(4, 4).T.copy()
@h
def glLoadMatrixf(p): cur()[-1] = np.frombuffer(rd(p, 64), '<f4').astype(np.float64).reshape(4, 4).T.copy()
@h
def glPushMatrix(): c = cur(); c.append(c[-1].copy())
@h
def glPopMatrix():
    c = cur()
    if len(c) > 1: c.pop()
@h
def glTranslatex(x, y, z): m = np.eye(4); m[:3, 3] = [fx(x), fx(y), fx(z)]; mul(m)
@h
def glScalex(x, y, z): mul(np.diag([fx(x), fx(y), fx(z), 1.0]))
@h
def glRotatex(a, x, y, z):
    a = np.radians(fx(a)); v = np.array([fx(x), fx(y), fx(z)]); n = np.linalg.norm(v)
    if n == 0: return
    x, y, z = v / n; c, s = np.cos(a), np.sin(a); t = 1 - c
    m = np.eye(4); m[:3, :3] = [[t*x*x+c, t*x*y-s*z, t*x*z+s*y], [t*x*y+s*z, t*y*y+c, t*y*z-s*x], [t*x*z-s*y, t*y*z+s*x, t*z*z+c]]; mul(m)
@h
def glOrthox(l, r, b, t):
    l, r, b, t, n, f = fx(l), fx(r), fx(b), fx(t), fx(sarg(0)), fx(sarg(1))
    m = np.eye(4); m[0, 0] = 2/(r-l); m[1, 1] = 2/(t-b); m[2, 2] = -2/(f-n); m[0, 3] = -(r+l)/(r-l); m[1, 3] = -(t+b)/(t-b); m[2, 3] = -(f+n)/(f-n); mul(m)
@h
def glFrustumx(l, r, b, t):
    l, r, b, t, n, f = fx(l), fx(r), fx(b), fx(t), fx(sarg(0)), fx(sarg(1))
    m = np.zeros((4, 4)); m[0, 0] = 2*n/(r-l); m[1, 1] = 2*n/(t-b); m[0, 2] = (r+l)/(r-l); m[1, 2] = (t+b)/(t-b); m[2, 2] = -(f+n)/(f-n); m[2, 3] = -2*f*n/(f-n); m[3, 2] = -1; mul(m)
@h
def glViewport(x, y, w, hh): gl.viewport = (s32(x), s32(y), w, hh)
@h
def glScissor(x, y, w, hh): gl.scissor = (s32(x), s32(y), w, hh)
@h
def glDepthRangex(n, f): gl.drange = (fx(n), fx(f))
@h
def glDepthRangef(n, f): gl.drange = (struct.unpack('<f', struct.pack('<I', n))[0], struct.unpack('<f', struct.pack('<I', f))[0])
@h
def glEnable(c):
    if c == 0x0DE1: gl.texen[gl.acttex] = True
    else: gl.en.add(c)
@h
def glDisable(c):
    if c == 0x0DE1: gl.texen[gl.acttex] = False
    else: gl.en.discard(c)
@h
def glActiveTexture(t): gl.acttex = min(1, t - 0x84C0)
@h
def glClientActiveTexture(t): gl.cacttex = min(1, t - 0x84C0)
@h
def glEnableClientState(a): gl.cen.add((a, gl.cacttex) if a == 0x8078 else a)
@h
def glDisableClientState(a): gl.cen.discard((a, gl.cacttex) if a == 0x8078 else a)
@h
def glVertexPointer(size, typ, stride, ptr): gl.arr[0x8074] = (size, typ, stride, ptr, gl.abuf)
@h
def glColorPointer(size, typ, stride, ptr): gl.arr[0x8076] = (size, typ, stride, ptr, gl.abuf)
@h
def glTexCoordPointer(size, typ, stride, ptr): gl.arr[(0x8078, gl.cacttex)] = (size, typ, stride, ptr, gl.abuf)
@h
def glNormalPointer(typ, stride, ptr): pass
@h
def glColor4x(r, g, b, a): gl.color = np.array([fx(r), fx(g), fx(b), fx(a)])
@h
def glBlendFunc(s, d): gl.blend = (s, d)
@h
def glAlphaFuncx(f, ref): gl.afunc = (f, fx(ref))
@h
def glDepthFunc(f): gl.dfunc = f
@h
def glDepthMask(m): gl.dmask = bool(m & 0xff)
@h
def glColorMask(r, g, b, a): gl.cmask = bool(r & 0xff)
@h
def glCullFace(m): gl.cull = m
@h
def glFrontFace(m): gl.front = m
@h
def glClearColorx(r, g, b, a): gl.clear = np.array([fx(r), fx(g), fx(b), fx(a)])
@h
def glClear(mask):
    if not gl.rendering: return
    if mask & 0x4000: gl.fb[:] = gl.clear
    if mask & 0x100: gl.zb[:] = 1.0
@h
def glBindBuffer(target, id_):
    if target == 0x8892: gl.abuf = id_
    else: gl.ebuf = id_
@h
def glBufferData(target, size, data, usage):
    gl.bufs[gl.abuf if target == 0x8892 else gl.ebuf] = rd(data, size) if data else bytes(size)
@h
def glDeleteBuffers(n, p):
    for i in range(n): gl.bufs.pop(r32(p + 4 * i), None)
@h
def glBindTexture(t, id_): gl.bound[gl.acttex] = id_; state.curtex = id_
@h
def glDeleteTextures(n, p):
    for i in range(n): gl.texdata.pop(r32(p + 4 * i), None)
@h
def glTexEnvx(target, pname, param):
    if pname == 0x2200: gl.texenv[gl.acttex] = param
@h
def glTexParameterx(target, pname, param): gl.texparam[(gl.bound[gl.acttex], pname)] = param

def decode(w, hh, fmt, typ, raw):
    n = w * hh
    if typ == 0x1401:
        a = np.frombuffer(raw, np.uint8).astype(np.float32) / 255.0
        if fmt == 0x1908: return a.reshape(hh, w, 4).copy()
        if fmt == 0x1907: return np.dstack([a.reshape(hh, w, 3), np.ones((hh, w), np.float32)])
        if fmt == 0x1909: l = a.reshape(hh, w); return np.dstack([l, l, l, np.ones_like(l)])
        if fmt == 0x1906: l = a.reshape(hh, w); return np.dstack([np.ones_like(l)] * 3 + [l])
        if fmt == 0x190A: la = a.reshape(hh, w, 2); return np.dstack([la[..., 0]] * 3 + [la[..., 1]])
    v = np.frombuffer(raw, '<u2').reshape(hh, w).astype(np.uint32)
    if typ == 0x8363: return np.dstack([(v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0, np.ones((hh, w))]).astype(np.float32)
    if typ == 0x8033: return np.dstack([(v >> 12) / 15.0, ((v >> 8) & 15) / 15.0, ((v >> 4) & 15) / 15.0, (v & 15) / 15.0]).astype(np.float32)
    if typ == 0x8034: return np.dstack([(v >> 11) / 31.0, ((v >> 6) & 31) / 31.0, ((v >> 1) & 31) / 31.0, (v & 1) * 1.0]).astype(np.float32)
    raise ValueError(f'texture format {fmt:#x}/{typ:#x}')
BPP = {(0x1908, 0x1401): 4, (0x1907, 0x1401): 3, (0x1909, 0x1401): 1, (0x1906, 0x1401): 1, (0x190A, 0x1401): 2}
def _tex2(target, level, ifmt, w):
    hh, border, fmt, typ, pix = sarg(0), sarg(1), sarg(2), sarg(3), sarg(4)
    stats['tex_uploads'] += 1; gl.fmts[(hex(fmt), hex(typ))] += 1
    if level != 0: return
    tid = gl.bound[gl.acttex]
    if pix: gl.texdata[tid] = decode(w, hh, fmt, typ, rd(pix, w * hh * BPP.get((fmt, typ), 2)))
    else: gl.texdata[tid] = np.zeros((hh, w, 4), np.float32)
    state.tex[tid] = (w, hh, fmt, typ)
H_['glTexImage2D'] = _tex2
def _texsub(target, level, xo, yo):
    w, hh, fmt, typ, pix = sarg(0), sarg(1), sarg(2), sarg(3), sarg(4)
    stats['tex_subuploads'] += 1
    t = gl.texdata.get(gl.bound[gl.acttex])
    if level == 0 and t is not None and pix:
        t[yo:yo + hh, xo:xo + w] = decode(w, hh, fmt, typ, rd(pix, w * hh * BPP.get((fmt, typ), 2)))
H_['glTexSubImage2D'] = _texsub

DT = {0x1400: ('i1', 1, 1.0), 0x1401: ('u1', 1, 1.0), 0x1402: ('<i2', 2, 1.0), 0x1403: ('<u2', 2, 1.0), 0x1406: ('<f4', 4, 1.0), 0x140C: ('<i4', 4, FX)}
def fetch(key, lo, hi, normalize=False):
    size, typ, stride, ptr, buf = gl.arr[key]
    dt, bs, sc = DT[typ]; st = stride or size * bs; n = hi - lo + 1
    if buf:
        raw = gl.bufs.get(buf, b'')[ptr + lo * st: ptr + lo * st + (n - 1) * st + size * bs]
    else:
        raw = rd(ptr + lo * st, (n - 1) * st + size * bs)
    need = (n - 1) * st + size * bs
    if len(raw) < need: raw = raw + bytes(need - len(raw))
    a = np.lib.stride_tricks.as_strided(np.frombuffer(raw, np.uint8), shape=(n, size * bs), strides=(st, 1))
    a = np.ascontiguousarray(a).view(dt).reshape(n, size).astype(np.float64) * sc
    if normalize and typ == 0x1401: a = a / 255.0
    return a

def factor(f, src, dst):
    if f == 0: return 0.0
    if f == 1: return 1.0
    if f == 0x300: return src
    if f == 0x301: return 1 - src
    if f == 0x302: return src[..., 3:4]
    if f == 0x303: return 1 - src[..., 3:4]
    if f == 0x304: return dst[..., 3:4]
    if f == 0x305: return 1 - dst[..., 3:4]
    if f == 0x306: return dst
    if f == 0x307: return 1 - dst
    return 1.0
def cmpf(f, a, b):
    return {0x200: lambda: np.zeros_like(a, bool), 0x201: lambda: a < b, 0x202: lambda: a == b, 0x203: lambda: a <= b, 0x204: lambda: a > b,
            0x205: lambda: a != b, 0x206: lambda: a >= b, 0x207: lambda: np.ones_like(a, bool)}[f]()

def sample(tid, u, v):
    t = gl.texdata.get(tid)
    if t is None: return np.ones(u.shape + (4,), np.float32)
    th, tw = t.shape[:2]
    return t[np.floor(v * th).astype(np.int64) % th, np.floor(u * tw).astype(np.int64) % tw]

def raster(P, C, UV):   # P: (3,4) clip coords, C: (3,4), UV: list of (3,2) or None
    vx, vy, vw, vh = gl.viewport
    w = P[:, 3]; ndc = P[:, :3] / w[:, None]
    sx = vx + (ndc[:, 0] + 1) * 0.5 * vw; sy = vy + (ndc[:, 1] + 1) * 0.5 * vh
    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
    if area == 0 or not np.isfinite(area): return
    if 0x0B44 in gl.en:
        front = (area > 0) == (gl.front == 0x901)
        if (gl.cull == 0x405 and not front) or (gl.cull == 0x404 and front): return
    x0 = max(0, int(np.floor(sx.min()))); x1 = min(W - 1, int(np.ceil(sx.max())))
    y0 = max(0, int(np.floor(sy.min()))); y1 = min(H - 1, int(np.ceil(sy.max())))
    if 0x0C11 in gl.en and gl.scissor:
        a, b, c, d = gl.scissor; x0 = max(x0, a); y0 = max(y0, b); x1 = min(x1, a + c - 1); y1 = min(y1, b + d - 1)
    if x1 < x0 or y1 < y0: return
    gl.tris += 1
    px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    b0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area
    b1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area
    b2 = 1 - b0 - b1
    m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
    if not m.any(): return
    z = b0 * ndc[0, 2] + b1 * ndc[1, 2] + b2 * ndc[2, 2]
    dn, df = gl.drange; depth = dn + (df - dn) * (z * 0.5 + 0.5)
    rows = slice(H - 1 - y1, H - y0); zb = gl.zb[rows, x0:x1 + 1][::-1]; fbv = gl.fb[rows, x0:x1 + 1][::-1]
    if 0x0B71 in gl.en: m &= cmpf(gl.dfunc, depth, zb)
    if not m.any(): return
    iw = np.stack([b0 / w[0], b1 / w[1], b2 / w[2]], -1); den = iw.sum(-1); den[den == 0] = 1e-12
    bc = iw / den[..., None]
    col = bc @ C
    for unit in (0, 1):
        if gl.texen[unit] and UV[unit] is not None:
            uv = bc @ UV[unit]
            tx = sample(gl.bound[unit], uv[..., 0], uv[..., 1]); env = gl.texenv[unit]
            if env == 0x1E01: col = tx.astype(np.float64)
            elif env == 0x2101: col = np.concatenate([col[..., :3] * (1 - tx[..., 3:4]) + tx[..., :3] * tx[..., 3:4], col[..., 3:4]], -1)
            elif env == 0x0104: col = np.concatenate([col[..., :3] + tx[..., :3], col[..., 3:4] * tx[..., 3:4]], -1)
            else: col = col * tx
    if 0x0BC0 in gl.en: m &= cmpf(gl.afunc[0], col[..., 3], gl.afunc[1])
    if not m.any(): return
    col = np.clip(col, 0, 1)
    if 0x0BE2 in gl.en:
        dst = fbv.astype(np.float64)
        col = np.clip(col * factor(gl.blend[0], col, dst) + dst * factor(gl.blend[1], col, dst), 0, 1)
    if gl.cmask: fbv[m] = col[m]
    if gl.dmask and 0x0B71 in gl.en: zb[m] = depth[m]

def clip_near(P, attrs):
    """Clip one triangle against w > eps. Returns list of (P, attrs)."""
    eps = 1e-5; inside = P[:, 3] > eps
    if inside.all(): return [(P, attrs)]
    if not inside.any(): return []
    pts = []
    for i in range(3):
        j = (i + 1) % 3
        if inside[i]: pts.append((P[i], [a[i] for a in attrs]))
        if inside[i] != inside[j]:
            t = (eps - P[i, 3]) / (P[j, 3] - P[i, 3])
            pts.append((P[i] + t * (P[j] - P[i]), [a[i] + t * (a[j] - a[i]) for a in attrs]))
    out = []
    for k in range(1, len(pts) - 1):
        tri = (pts[0], pts[k], pts[k + 1])
        out.append((np.array([p[0] for p in tri]), [np.array([p[1][n] for p in tri]) for n in range(len(attrs))]))
    return out

def draw(mode, idx):
    gl.modes[mode] += 1
    if not gl.rendering or mode not in (4, 5, 6) or 0x8074 not in gl.cen or len(idx) < 3: return
    lo, hi = int(idx.min()), int(idx.max())
    pos = fetch(0x8074, lo, hi); n = len(pos)
    p4 = np.zeros((n, 4)); p4[:, 3] = 1; p4[:, :pos.shape[1]] = pos
    clip = p4 @ (gl.mats[0x1701][-1] @ gl.mats[0x1700][-1]).T
    if 0x8076 in gl.cen:
        c = fetch(0x8076, lo, hi, True); col = np.ones((n, 4)); col[:, :c.shape[1]] = c
    else: col = np.tile(gl.color, (n, 1))
    uvs = []
    for unit in (0, 1):
        if (0x8078, unit) in gl.cen and gl.texen[unit]:
            t = fetch((0x8078, unit), lo, hi); t4 = np.zeros((n, 4)); t4[:, 3] = 1; t4[:, :t.shape[1]] = t
            tm = gl.mats[0x1702 if unit == 0 else 'tex1'][-1]
            uvs.append((t4 @ tm.T)[:, :2])
        else: uvs.append(None)
    if os.environ.get('DBG_DRAW'):
        w_ = clip[:, 3:4]; nd = clip[:, :3] / w_
        print(f'   [draw] vp={gl.viewport} n={n} idx={len(idx)} varr={gl.arr[0x8074][:3]} pos x[{pos[:,0].min():.1f},{pos[:,0].max():.1f}] y[{pos[:,1].min():.1f},{pos[:,1].max():.1f}] ndc x[{nd[:,0].min():.2f},{nd[:,0].max():.2f}] y[{nd[:,1].min():.2f},{nd[:,1].max():.2f}] tex={gl.bound} texen={gl.texen} uv0={None if uvs[0] is None else (uvs[0].min(0).round(2).tolist(), uvs[0].max(0).round(2).tolist())}')
        print('      proj diag', np.round(np.diag(gl.mats[0x1701][-1]),4).tolist(), 'mv diag', np.round(np.diag(gl.mats[0x1700][-1]),4).tolist(), 'tex', None if gl.texdata.get(gl.bound[0]) is None else gl.texdata[gl.bound[0]].shape)
    idx = idx - lo
    if mode == 4: tris = idx[:len(idx) // 3 * 3].reshape(-1, 3)
    elif mode == 5:
        k = np.arange(len(idx) - 2); tris = np.stack([idx[k], idx[k + 1], idx[k + 2]], 1); odd = k % 2 == 1
        tris[odd] = tris[odd][:, [1, 0, 2]]
    else:
        k = np.arange(1, len(idx) - 1); tris = np.stack([np.full(len(k), idx[0]), idx[k], idx[k + 1]], 1)
    zero2 = np.zeros((3, 2))
    for t in tris:
        if t[0] == t[1] or t[1] == t[2] or t[0] == t[2]: continue
        P = clip[t]
        attrs = [col[t]] + [(u[t] if u is not None else zero2) for u in uvs]
        for P2, a2 in clip_near(P, attrs):
            raster(P2, a2[0], [a2[1] if uvs[0] is not None else None, a2[2] if uvs[1] is not None else None])

@h
def glDrawArrays(mode, first, count):
    state.draws += 1; state.verts += count; stats['draw_calls'] += 1
    draw(mode, np.arange(first, first + count))
@h
def glDrawElements(mode, count, typ, idx):
    state.draws += 1; state.verts += count; stats['draw_calls'] += 1
    if not gl.rendering: gl.modes[mode] += 1; return
    bs = 2 if typ == 0x1403 else 1
    raw = gl.bufs.get(gl.ebuf, b'')[idx: idx + count * bs] if gl.ebuf else rd(idx, count * bs)
    draw(mode, np.frombuffer(raw, '<u2' if bs == 2 else 'u1').astype(np.int64))

def save_frame(tag=None):
    img = (np.clip(gl.fb[..., :3], 0, 1) * 255).astype(np.uint8)
    p = os.path.join(OUT, f'frame_{state.frames:05d}.png'); Image.fromarray(img).save(p)
    print(f'   [render] saved {p}  ({gl.tris} triangles rasterised, {state.draws} draw calls this frame)')
def want(n): return n in RENDER_FRAMES or (RENDER_EVERY and n % RENDER_EVERY == 0)
_orig_swap = H_['eglSwapBuffers']
def _swap(d, s):
    if gl.rendering: save_frame()
    r = _orig_swap(d, s)
    gl.rendering = want(state.frames + 1); gl.tris = 0
    return r
H_['eglSwapBuffers'] = _swap
gl.rendering = want(1)
