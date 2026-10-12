# GL call argument survey, exec'd inside emu.py's namespace after glsoft.py (GLSURVEY=1)
survey = collections.Counter()
SKIP = {'glDrawElements', 'glDrawArrays', 'glTexImage2D', 'glTexSubImage2D', 'glLoadMatrixx', 'glLoadMatrixf',
        'glBufferData', 'glGenTextures', 'glGenBuffers', 'glDeleteTextures', 'glDeleteBuffers', 'glBindTexture',
        'glBindBuffer', 'glTranslatex', 'glRotatex', 'glScalex', 'glOrthox', 'glFrustumx', 'glColor4x',
        'glViewport', 'glScissor', 'glDepthRangex', 'glPushMatrix', 'glPopMatrix', 'glLoadIdentity'}
for _n in syms:
    if _n.startswith('gl') and _n not in SKIP:
        _f = H_.get(_n)
        if _f is None:
            _f = (lambda *a: 0)
        def _mk(name, f):
            def w(a0=0, a1=0, a2=0, a3=0):
                args = (a0, a1, a2, a3)
                key = args[:3] if name.endswith('Pointer') else args
                if name.endswith('v') or name in ('glGetIntegerv',):
                    key = (args[0], args[1]) if name != 'glGetIntegerv' else (args[0],)
                    if name in ('glFogxv', 'glLightxv', 'glMaterialxv', 'glLightModelxv', 'glTexEnvxv'):
                        p = args[2] if name in ('glLightxv', 'glMaterialxv', 'glTexEnvxv') else args[1]
                        key = key + tuple(s32(r32(p + 4 * i)) / 65536 for i in range(4))
                survey[(name,) + tuple(hex(x) if isinstance(x, int) else x for x in key)] += 1
                n = f.__code__.co_argcount if hasattr(f, '__code__') and not (f.__code__.co_flags & 0x04) else 4
                return f(*args[:n])
            return w
        H_[_n] = _mk(_n, _f)
import atexit
def _dump():
    with open(os.path.join(OUT, 'glsurvey.txt'), 'w') as fo:
        for k, v in sorted(survey.items(), key=lambda kv: (kv[0][0], -kv[1])):
            fo.write(f'{v:8d} {" ".join(map(str, k))}\n')
atexit.register(_dump)
